/*
 * hidwayd - HIDway relay for the Raspberry Pi.
 *
 * Receives full-state input packets from the client over UDP (through
 * Tailscale), keeps only the latest, and forwards it to the Pico over a
 * serial link. It echoes a status packet back to the client for RTT and loss
 * measurement.
 *
 * Principles: accept only from the allowed peer; never queue (keep the latest
 * state only); treat loss of the client as "release everything".
 *
 * Serial link: the device may come and go (probe unplugged, Pico reset). The
 * relay keeps running, retries the open once per second and starts every new
 * connection with a RELEASE so the Pico begins from a known state. While the
 * tty still holds an unsent frame, newer states replace the pending one
 * instead of queueing behind it. A RELEASE discards anything pending and is
 * always written.
 *
 * Encryption (optional): with --key-file the relay accepts only sealed
 * envelopes (common/hidway_crypto.h, XChaCha20-Poly1305 with a pre-shared
 * key), rejects replays by timestamp and seals its status replies, so a
 * network path that terminates encryption (e.g. Cloudflare) sees only
 * ciphertext. Without a key it speaks the plaintext protocol as before and
 * ignores sealed packets.
 */
#define _GNU_SOURCE /* cfmakeraw, CRTSCTS, getopt_long, clock_gettime */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "hidway_crypto.h"
#include "protocol.h"
#include "serial.h"

#ifndef HIDWAY_VERSION
#define HIDWAY_VERSION "dev"
#endif

#define LINK_TIMEOUT_MS   250
#define TICK_MS           1000 /* counters/log while a session is live */
#define SERIAL_RETRY_MS   1000
#define RATE_LIMIT_PPS    2000 /* accepted packets per second, RELEASE exempt */
#define POLL_PENDING_MS   1    /* a frame is waiting for the tty to drain */

#define TARGET_STALE_MS   10000 /* no report from the Pico for this long: state unknown */
#define WAKE_MIN_GAP_MS   3000  /* at most one Wake-on-LAN burst per this, on request */

/* Sealed packets must be within this of the relay's wall clock (both ends run
 * NTP); together with "strictly newer" it bounds replays after a restart. */
#define REPLAY_WINDOW_US (120ull * 1000000ull)

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint64_t wall_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

/* ------------------------------------------------------------- encryption */

static struct {
    bool on;
    uint8_t key[HIDWAY_KEY_LEN];
    hidway_replay_t replay;
    bool warned_plain;  /* plaintext seen while encryption is required */
    bool warned_sealed; /* sealed seen while no key is configured */
    bool warned_skew;
    uint32_t rejected;  /* forged, tampered, replayed or wrong-mode packets */
} cry;

static bool fill_random(uint8_t *p, size_t n)
{
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static int gen_key(void)
{
    uint8_t k[HIDWAY_KEY_LEN];
    char hex[2 * HIDWAY_KEY_LEN + 1];
    if (!fill_random(k, sizeof k)) {
        perror("getrandom");
        return 1;
    }
    hidway_key_format(k, hex);
    printf("%s\n", hex);
    hidway_wipe(k, sizeof k);
    hidway_wipe(hex, sizeof hex);
    return 0;
}

/* ------------------------------------------------------------ wake-on-LAN */

static bool parse_mac(const char *s, uint8_t mac[6])
{
    if (strlen(s) != 17)
        return false;
    for (int i = 0; i < 6; i++) {
        const char *p = s + 3 * i;
        if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) ||
            (i < 5 && p[2] != ':' && p[2] != '-'))
            return false;
        char byte[3] = {p[0], p[1], 0};
        mac[i] = (uint8_t)strtoul(byte, NULL, 16);
    }
    return true;
}

/* A magic packet (6 x 0xFF, then the MAC 16 times) and where it goes: the
 * target's broadcast address on this network, or a router that forwards the
 * port to the target. Used by --wake and, in the relay, by the client's WAKE. */
static struct {
    bool on;
    char mac_text[18];
    uint8_t pkt[6 + 16 * 6];
    struct sockaddr_in dst;
    uint64_t last_ms; /* relay: last burst sent on request */
    bool warned;      /* relay: WAKE asked for but none configured */
} wake;

/* `to` is "IPv4[:PORT]"; NULL or empty means the local broadcast on port 9. */
static bool wake_setup(const char *mac_text, const char *to)
{
    uint8_t mac[6];
    if (!parse_mac(mac_text, mac)) {
        fprintf(stderr, "hidwayd: bad MAC address: %s (use aa:bb:cc:dd:ee:ff)\n", mac_text);
        return false;
    }
    char host[64];
    int port = 9;
    snprintf(host, sizeof host, "%s", to && *to ? to : "255.255.255.255");
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }
    memset(&wake.dst, 0, sizeof wake.dst);
    wake.dst.sin_family = AF_INET;
    wake.dst.sin_port = htons((uint16_t)port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, host, &wake.dst.sin_addr) != 1) {
        fprintf(stderr, "hidwayd: bad wake address: %s (use IPv4[:PORT])\n", to ? to : host);
        return false;
    }
    memset(wake.pkt, 0xFF, 6);
    for (int i = 0; i < 16; i++)
        memcpy(wake.pkt + 6 + 6 * i, mac, 6);
    snprintf(wake.mac_text, sizeof wake.mac_text, "%s", mac_text);
    wake.on = true;
    return true;
}

/* Three copies, since UDP may drop one, `gap_ms` apart. Returns how many went out. */
static int wake_send(long gap_ms)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int on = 1;
    if (s < 0 || setsockopt(s, SOL_SOCKET, SO_BROADCAST, &on, sizeof on) != 0) {
        if (s >= 0)
            close(s);
        return 0;
    }
    int sent = 0;
    for (int i = 0; i < 3; i++) {
        if (i && gap_ms) {
            struct timespec gap = {0, gap_ms * 1000000L};
            nanosleep(&gap, NULL);
        }
        if (sendto(s, wake.pkt, sizeof wake.pkt, 0, (const struct sockaddr *)&wake.dst, sizeof wake.dst) ==
            (ssize_t)sizeof wake.pkt)
            sent++;
    }
    close(s);
    return sent;
}

static void wake_describe(char *out, size_t cap)
{
    char a[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &wake.dst.sin_addr, a, sizeof a);
    snprintf(out, cap, "%s via %s:%u", wake.mac_text, a, ntohs(wake.dst.sin_port));
}

/* --wake: one-shot. */
static int wake_target(const char *mac_text, const char *to)
{
    if (!wake_setup(mac_text, to))
        return 2;
    if (!wake_send(100)) {
        perror("hidwayd: wake");
        return 1;
    }
    char d[64];
    wake_describe(d, sizeof d);
    printf("hidwayd: magic packet sent: %s\n", d);
    return 0;
}

/* WAKE from the client (already authenticated and from the allowed peer):
 * only ever this configured packet, at most once per WAKE_MIN_GAP_MS. */
static void handle_wake(uint64_t t)
{
    if (!wake.on) {
        if (!wake.warned)
            fprintf(stderr, "hidwayd: wake requested, but HIDWAY_WAKE_MAC is not set\n");
        wake.warned = true;
        return;
    }
    if (wake.last_ms && t - wake.last_ms < WAKE_MIN_GAP_MS)
        return;
    wake.last_ms = t;
    char d[64];
    wake_describe(d, sizeof d);
    fprintf(stderr, "hidwayd: wake requested: magic packet %s (%s)\n", wake_send(0) ? "sent" : "FAILED", d);
}

static bool load_key_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "hidwayd: cannot read key file %s: %s\n", path, strerror(errno));
        return false;
    }
    /* root:hidway 0640 (service group may read) is the intended setup; warn
     * about anything wider. */
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && (st.st_mode & 0037))
        fprintf(stderr, "hidwayd: WARNING key file %s is writable by its group or readable by "
                        "others (use chmod 640)\n", path);
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    bool ok = hidway_key_parse(buf, cry.key);
    hidway_wipe(buf, sizeof buf);
    if (!ok)
        fprintf(stderr, "hidwayd: key file %s must hold exactly 64 hex characters\n", path);
    return ok;
}

/* Turn a received datagram into an input packet, enforcing the configured
 * mode: with a key only authentic, fresh, sealed packets pass. */
static bool relay_decode(const uint8_t *buf, size_t n, hidway_input_pkt_t *p)
{
    if (!cry.on) {
        if (hidway_is_sealed(buf, n)) {
            if (!cry.warned_sealed) {
                fprintf(stderr, "hidwayd: client sends encrypted packets but no --key-file is set\n");
                cry.warned_sealed = true;
            }
            return false;
        }
        return hidway_input_decode(buf, n, p);
    }

    if (!hidway_is_sealed(buf, n)) {
        if (!cry.warned_plain) {
            fprintf(stderr, "hidwayd: plaintext packet rejected (encryption is required)\n");
            cry.warned_plain = true;
        }
        cry.rejected++;
        return false;
    }
    uint8_t inner[HIDWAY_SEAL_MAX_INNER];
    uint64_t ts = 0;
    size_t il = hidway_open(inner, sizeof inner, cry.key, buf, n, &ts);
    if (!il) {
        cry.rejected++; /* forged, tampered or sealed with another key */
        return false;
    }
    uint64_t now = wall_us();
    if (!hidway_replay_accept(&cry.replay, ts, now, REPLAY_WINDOW_US)) {
        cry.rejected++;
        long long skew_s = ((long long)ts - (long long)now) / 1000000;
        if (!cry.warned_skew && (skew_s > 120 || skew_s < -120)) {
            fprintf(stderr, "hidwayd: rejecting packets: client clock is %+lld s off "
                            "(sync both clocks with NTP)\n", skew_s);
            cry.warned_skew = true;
        }
        hidway_wipe(inner, sizeof inner);
        return false;
    }
    cry.warned_skew = false;
    bool ok = hidway_input_decode(inner, il, p);
    hidway_wipe(inner, sizeof inner);
    return ok;
}

static void send_status(int fd, const hidway_status_pkt_t *st, const struct sockaddr_in *to,
                        socklen_t tolen)
{
    uint8_t sb[HIDWAY_STATUS_PKT_LEN];
    size_t sl = hidway_status_encode(sb, st);
    if (!cry.on) {
        sendto(fd, sb, sl, 0, (const struct sockaddr *)to, tolen);
        return;
    }
    uint8_t nonce[HIDWAY_NONCE_LEN], env[HIDWAY_SEAL_MAX];
    if (!fill_random(nonce, sizeof nonce))
        return;
    size_t el = hidway_seal(env, sizeof env, cry.key, nonce, wall_us(), sb, sl);
    if (el)
        sendto(fd, env, el, 0, (const struct sockaddr *)to, tolen);
}

/* ------------------------------------------------------------ serial link */

typedef struct {
    const char *dev;      /* NULL: no serial configured (dump/echo mode) */
    int baud;
    int fd;               /* -1 while closed */
    int last_errno;       /* last open failure, to log only on change */
    uint64_t next_try_ms; /* next open attempt while closed */
    bool have_pending;    /* a frame is waiting for the tty to drain */
    bool resync;          /* a frame went out partially; start the next with 0x00 */
    hidway_serial_state_t pending;
    uint32_t replaced;    /* states superseded before they were written */
    hidway_deframer_t rx; /* reports from the Pico */
    uint8_t target;       /* last HIDWAY_TARGET_* the Pico reported */
    uint64_t target_ms;   /* when; 0 = no fresh report */
} serial_link_t;

static speed_t baud_const(int baud)
{
    switch (baud) {
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 1000000: return B1000000;
    default: return 0;
    }
}

static void serial_close(serial_link_t *l, uint64_t t)
{
    close(l->fd);
    l->fd = -1;
    l->have_pending = false;
    l->resync = false;
    l->next_try_ms = t + SERIAL_RETRY_MS;
    l->target_ms = 0;
}

/* Target reports from the Pico (it sends one on every change and every few
 * seconds while it has power). */
static void serial_read(serial_link_t *l, uint64_t t)
{
    uint8_t buf[64];
    ssize_t n;
    while (l->fd >= 0 && (n = read(l->fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            hidway_serial_state_t s;
            if (hidway_deframer_push(&l->rx, buf[i], &s) && s.type == HIDWAY_SER_TARGET) {
                l->target = s.target;
                l->target_ms = t ? t : 1;
            }
        }
    }
}

/* Forget a report that has gone stale (the Pico lost power or the link). */
static void target_expire(serial_link_t *l, uint64_t t)
{
    if (l->target_ms && t - l->target_ms > TARGET_STALE_MS)
        l->target_ms = 0;
}

static uint8_t target_state(const serial_link_t *l)
{
    return l->fd >= 0 && l->target_ms ? l->target : HIDWAY_TARGET_UNKNOWN;
}

static const char *target_name(uint8_t t)
{
    return t == HIDWAY_TARGET_ON ? "on" : t == HIDWAY_TARGET_ASLEEP ? "asleep" : t == HIDWAY_TARGET_OFF ? "off" : "unknown";
}

/* Write one frame without blocking. Returns true once it is fully written.
 * If only part of it went out, the next frame starts with a delimiter so the
 * receiver drops the fragment (CRC) and decodes the next frame cleanly. On a
 * hard error the link is closed and reopened later. */
static bool serial_write_frame(serial_link_t *l, const hidway_serial_state_t *s)
{
    uint8_t f[1 + HIDWAY_SER_FRAME_MAX];
    f[0] = 0x00;
    size_t n = 1 + hidway_serial_build(s, f + 1, sizeof f - 1);
    size_t start = l->resync ? 0 : 1;
    size_t off = start;
    while (off < n) {
        ssize_t w = write(l->fd, f + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (off > start)
                    l->resync = true;
                return false; /* kept pending, retried shortly */
            }
            fprintf(stderr, "hidwayd: serial write failed (%s), closing\n", strerror(errno));
            serial_close(l, now_ms());
            return false;
        }
        off += (size_t)w;
    }
    l->resync = false;
    return true;
}

/* Write the pending frame once the tty has nothing left to send. */
static void serial_pump(serial_link_t *l)
{
    if (!l->have_pending || l->fd < 0)
        return;
    int outq = 0;
    if (ioctl(l->fd, TIOCOUTQ, &outq) == 0 && outq > 0)
        return;
    if (serial_write_frame(l, &l->pending))
        l->have_pending = false;
}

/* Release everything: discard whatever is still queued and send RELEASE
 * ahead of anything else. */
static void serial_send_release(serial_link_t *l, uint16_t seq)
{
    l->have_pending = false;
    if (l->fd < 0)
        return;
    tcflush(l->fd, TCOFLUSH);
    l->resync = true; /* the flush may have cut a frame short */
    memset(&l->pending, 0, sizeof l->pending);
    l->pending.type = HIDWAY_SER_RELEASE;
    l->pending.seq = seq;
    l->have_pending = true;
    serial_pump(l);
}

static void serial_open(serial_link_t *l, uint64_t t)
{
    if (!l->dev || l->fd >= 0 || t < l->next_try_ms)
        return;
    l->next_try_ms = t + SERIAL_RETRY_MS;

    int fd = open(l->dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (errno != l->last_errno)
            fprintf(stderr, "hidwayd: serial %s unavailable (%s), retrying every %d ms\n",
                    l->dev, strerror(errno), SERIAL_RETRY_MS);
        l->last_errno = errno;
        return;
    }
    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        cfsetospeed(&tio, baud_const(l->baud));
        cfsetispeed(&tio, baud_const(l->baud));
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= (tcflag_t)~CRTSCTS;
        tcsetattr(fd, TCSANOW, &tio);
    }
    l->fd = fd;
    l->last_errno = 0;
    hidway_deframer_reset(&l->rx);
    l->target_ms = 0;
    fprintf(stderr, "hidwayd: serial %s open at %d baud\n", l->dev, l->baud);
    serial_send_release(l, 0); /* every connection starts from "nothing held" */
}

static void serial_send_state(serial_link_t *l, const hidway_input_pkt_t *p)
{
    if (l->fd < 0)
        return;
    if (l->have_pending)
        l->replaced++;
    hidway_serial_state_t *s = &l->pending;
    s->type = HIDWAY_SER_STATE;
    s->seq = (uint16_t)p->seq;
    s->mods = p->mods;
    memcpy(s->keys, p->keys, HIDWAY_KEY_BITMAP_BYTES);
    s->buttons = p->buttons;
    s->x = p->x;
    s->y = p->y;
    s->wheel = p->wheel;
    s->pan = p->pan;
    l->have_pending = true;
    serial_pump(l);
}

/* ------------------------------------------------------------ state file */

/* What the state file publishes for local tooling (hidway-status,
 * hidway-update, monitoring). */
typedef struct {
    bool link_up;
    bool have_session;
    uint32_t session;
    uint32_t frames_ok;
    uint16_t seq_gaps;
    uint32_t pps;        /* accepted packets in the last second */
    uint32_t rate_drops;
    time_t started;      /* wall clock */
    time_t last_rx;      /* wall clock of the last accepted packet, 0 = never */
} relay_view_t;

/* Written atomically (tmp + rename) on every change and once per second while
 * a session is live; never while idle. Removed on exit. */
static void write_state_file(const char *path, const relay_view_t *v, const serial_link_t *l)
{
    if (!path)
        return;
    char tmp[512];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return;
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fprintf(f,
            "version=%s\npid=%ld\nstarted=%lld\nlink=%s\nserial=%s\nserial_dev=%s\n"
            "last_rx=%lld\npps=%u\nframes=%u\ngaps=%u\nrate_drops=%u\nreplaced=%u\n",
            HIDWAY_VERSION, (long)getpid(), (long long)v->started, v->link_up ? "up" : "down",
            !l->dev ? "none" : l->fd >= 0 ? "open" : "closed", l->dev ? l->dev : "",
            (long long)v->last_rx, v->pps, v->frames_ok, v->seq_gaps, v->rate_drops, l->replaced);
    fprintf(f, "crypto=%s\ncrypto_rejected=%u\n", cry.on ? "on" : "off", cry.rejected);
    fprintf(f, "target=%s\nwake=%s\n", target_name(target_state(l)), wake.on ? "on" : "off");
    if (v->have_session)
        fprintf(f, "session=%08x\n", v->session);
    if (fclose(f) == 0)
        rename(tmp, path);
    else
        unlink(tmp);
}

/* Sleep until the next thing that needs doing: a pending serial frame, the
 * link timeout, the per-second tick of a live session, or the next serial
 * open attempt. Idle with the serial device open: no wakeups at all. */
static int poll_timeout(uint64_t t, bool link_up, uint64_t last_pkt_ms, uint64_t next_tick_ms,
                        const serial_link_t *l)
{
    if (l->have_pending)
        return POLL_PENDING_MS;
    uint64_t deadline = UINT64_MAX;
    if (link_up) {
        uint64_t timeout_at = last_pkt_ms + LINK_TIMEOUT_MS + 1;
        deadline = timeout_at < next_tick_ms ? timeout_at : next_tick_ms;
    }
    if (l->dev && l->fd < 0 && l->next_try_ms < deadline)
        deadline = l->next_try_ms;
    if (l->fd >= 0 && l->target_ms && l->target_ms + TARGET_STALE_MS + 1 < deadline)
        deadline = l->target_ms + TARGET_STALE_MS + 1; /* to notice the reports stopping */
    if (deadline == UINT64_MAX)
        return -1;
    return deadline > t ? (int)(deadline - t) : 0;
}

/* ------------------------------------------------------------ rate limit */

/* Fixed one-second window. Real input is at most ~1 kHz, so the cap only
 * bounds a misbehaving or hostile sender. */
typedef struct {
    uint64_t window_ms;
    uint32_t count;
    uint32_t drops;
} rate_limit_t;

static bool rate_allow(rate_limit_t *r, uint64_t t)
{
    if (t - r->window_ms >= 1000) {
        r->window_ms = t;
        r->count = 0;
    }
    if (r->count >= RATE_LIMIT_PPS) {
        r->drops++;
        return false;
    }
    r->count++;
    return true;
}

static int popcount_bitmap(const uint8_t *bm, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t b = bm[i]; b; b &= (uint8_t)(b - 1))
            c++;
    return c;
}

static uint16_t status_flags(const serial_link_t *l)
{
    return (uint16_t)((l->fd >= 0 ? HIDWAY_STATUS_SERIAL_OPEN : 0) | (cry.on ? HIDWAY_STATUS_ENCRYPTED : 0) |
                      (wake.on ? HIDWAY_STATUS_WAKE : 0) |
                      (target_state(l) << HIDWAY_STATUS_TARGET_SHIFT));
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--bind IP] [--allow IP] [--port N] [--serial DEV] [--baud N]\n"
            "          [--state-file PATH] [--key-file PATH] [--quiet] [--version]\n"
            "       %s --gen-key   print a new random 32-byte key (64 hex chars)\n"
            "       %s --wake MAC [--wake-to IPv4[:PORT]]   send a Wake-on-LAN packet\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    const char *bind_addr = "0.0.0.0";
    const char *allow_addr = NULL; /* if set, only accept this source IP */
    const char *state_path = NULL; /* if set, publish link/serial state here */
    const char *key_path = NULL;   /* if set, require sealed packets */
    const char *wake_mac = NULL, *wake_to = NULL; /* --wake: send a magic packet and exit */
    int port = 47800;
    int quiet = 0;
    serial_link_t ser = {.baud = 921600, .fd = -1};

    static const struct option opts[] = {
        {"bind", required_argument, 0, 'b'},
        {"allow", required_argument, 0, 'a'},
        {"port", required_argument, 0, 'p'},
        {"serial", required_argument, 0, 's'},
        {"baud", required_argument, 0, 'B'},
        {"state-file", required_argument, 0, 'S'},
        {"key-file", required_argument, 0, 'k'},
        {"gen-key", no_argument, 0, 'G'},
        {"wake", required_argument, 0, 'W'},
        {"wake-to", required_argument, 0, 'T'},
        {"quiet", no_argument, 0, 'q'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "b:a:p:s:B:S:k:GW:T:qV", opts, NULL)) != -1) {
        switch (c) {
        case 'b': bind_addr = optarg; break;
        case 'a': allow_addr = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 's': ser.dev = optarg; break;
        case 'B': ser.baud = atoi(optarg); break;
        case 'S': state_path = optarg; break;
        case 'k': key_path = optarg; break;
        case 'G': return gen_key();
        case 'W': wake_mac = optarg; break;
        case 'T': wake_to = optarg; break;
        case 'q': quiet = 1; break;
        case 'V': printf("hidwayd %s\n", HIDWAY_VERSION); return 0;
        default: usage(argv[0]); return 2;
        }
    }
    if (wake_mac)
        return wake_target(wake_mac, wake_to);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "bad port: %d\n", port);
        return 2;
    }
    if (ser.dev && !baud_const(ser.baud)) {
        fprintf(stderr, "unsupported baud: %d\n", ser.baud);
        return 2;
    }
    if (key_path) {
        if (!load_key_file(key_path))
            return 2; /* never fall back to plaintext when a key was asked for */
        cry.on = true;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1) {
        fprintf(stderr, "bad bind address: %s\n", bind_addr);
        return 2;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { perror("bind"); return 1; }

    struct in_addr allow_in = {0};
    if (allow_addr && inet_pton(AF_INET, allow_addr, &allow_in) != 1) {
        fprintf(stderr, "bad allow address: %s\n", allow_addr);
        return 2;
    }

    fprintf(stderr, "hidwayd %s: listening on %s:%d%s%s | serial: %s\n", HIDWAY_VERSION,
            bind_addr, port, allow_addr ? ", allow=" : "", allow_addr ? allow_addr : "",
            ser.dev ? ser.dev : "none (dump/echo mode)");
    if (!allow_addr)
        fprintf(stderr, "hidwayd: WARNING no --allow set, accepting any source\n");
    fprintf(stderr, "hidwayd: encryption %s\n",
            cry.on ? "ON (XChaCha20-Poly1305, sealed packets only)" : "off (plaintext protocol)");

    /* Wake-on-LAN on the client's request: from hidwayd.conf, which the
     * service loads into the environment. */
    const char *wake_env = getenv("HIDWAY_WAKE_MAC");
    if (wake_env && *wake_env) {
        if (wake_setup(wake_env, getenv("HIDWAY_WAKE_TO"))) {
            char d[64];
            wake_describe(d, sizeof d);
            fprintf(stderr, "hidwayd: wake-on-LAN on (%s)\n", d);
        } else {
            fprintf(stderr, "hidwayd: wake-on-LAN off: fix HIDWAY_WAKE_MAC / HIDWAY_WAKE_TO\n");
        }
    }

    serial_open(&ser, now_ms());

    /* Per-session accounting. */
    uint32_t session = 0;
    uint32_t last_seq = 0;
    bool have_session = false;
    uint32_t frames_ok = 0;
    uint16_t seq_gaps = 0;
    hidway_input_pkt_t latest = {0};

    bool link_up = false;
    uint64_t last_pkt_ms = 0;
    uint64_t next_tick_ms = 0; /* per-second tick, only while the link is up */
    uint32_t in_this_sec = 0, in_pps = 0;
    rate_limit_t rate = {.window_ms = now_ms()};
    time_t started = time(NULL);

    /* Published state; the file is rewritten when one of these changes. */
    bool pub_link = false, pub_serial = ser.fd >= 0, pub_session = false;
    uint8_t pub_target = HIDWAY_TARGET_UNKNOWN;
    uint32_t pub_session_id = 0;
    bool publish = false;
    relay_view_t initial = {.started = started};
    write_state_file(state_path, &initial, &ser);

    while (running) {
        struct pollfd pfd[2] = {
            {.fd = fd, .events = POLLIN},
            {.fd = ser.fd, .events = POLLIN}, /* target reports; POLLHUP/POLLERR: device gone */
        };
        int pr = poll(pfd, ser.fd >= 0 ? 2 : 1,
                      poll_timeout(now_ms(), link_up, last_pkt_ms, next_tick_ms, &ser));
        uint64_t t = now_ms();

        if (pr > 0 && ser.fd >= 0 && (pfd[1].revents & (POLLHUP | POLLERR | POLLNVAL))) {
            fprintf(stderr, "hidwayd: serial %s disconnected\n", ser.dev);
            serial_close(&ser, t);
        }
        if (pr > 0 && ser.fd >= 0 && (pfd[1].revents & POLLIN))
            serial_read(&ser, t);
        target_expire(&ser, t);

        if (pr > 0 && (pfd[0].revents & POLLIN)) {
            uint8_t buf[128];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof from;
            ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
            hidway_input_pkt_t p;
            if (n < 0) {
                if (errno != EINTR) perror("recvfrom");
            } else if (allow_addr && from.sin_addr.s_addr != allow_in.s_addr) {
                /* not our peer: drop silently */
            } else if (!relay_decode(buf, (size_t)n, &p)) {
                /* malformed, forged, replayed or wrong mode: drop */
            } else if (p.type != HIDWAY_MSG_RELEASE && !rate_allow(&rate, t)) {
                /* over the sanity cap: drop (a RELEASE always passes) */
            } else if (p.type == HIDWAY_MSG_PROBE || p.type == HIDWAY_MSG_WAKE) {
                /* Reachability ping (and wake request): reply, but do not
                 * touch link state, the latest input, the serial link or
                 * the counters. */
                if (p.type == HIDWAY_MSG_WAKE)
                    handle_wake(t);
                hidway_status_pkt_t st = {0};
                st.session_id = p.session_id;
                st.seq = last_seq;
                st.client_time_us = p.client_time_us;
                st.frames_ok = frames_ok;
                st.seq_gaps = seq_gaps;
                st.flags = status_flags(&ser);
                send_status(fd, &st, &from, fromlen);
            } else {
                bool accept = true;
                if (!have_session || p.session_id != session) {
                    session = p.session_id;
                    have_session = true;
                    last_seq = p.seq; /* baseline; no gap for the first */
                    frames_ok = 0;
                    seq_gaps = 0;
                    fprintf(stderr, "hidwayd: new session %08x\n", session);
                } else if (p.seq > last_seq) {
                    seq_gaps = (uint16_t)(seq_gaps + (p.seq - last_seq - 1));
                    last_seq = p.seq;
                } else {
                    /* stale or duplicate: cumulative state makes this safe
                     * to drop entirely */
                    accept = false;
                }

                if (accept) {
                    frames_ok++;
                    in_this_sec++;
                    latest = p;
                    if (!link_up) {
                        link_up = true;
                        next_tick_ms = t + TICK_MS;
                    }
                    last_pkt_ms = t;

                    /* Forward the latest full state to the Pico. */
                    if (p.type == HIDWAY_MSG_RELEASE)
                        serial_send_release(&ser, (uint16_t)p.seq);
                    else
                        serial_send_state(&ser, &latest);

                    /* Echo status back for RTT/loss measurement. */
                    hidway_status_pkt_t st = {0};
                    st.session_id = session;
                    st.seq = last_seq;
                    st.client_time_us = p.client_time_us;
                    st.frames_ok = frames_ok;
                    st.seq_gaps = seq_gaps;
                    st.flags = status_flags(&ser);
                    st.leds = 0;
                    send_status(fd, &st, &from, fromlen);
                }
            }
        }

        if (link_up && t - last_pkt_ms > LINK_TIMEOUT_MS) {
            link_up = false;
            fprintf(stderr, "hidwayd: LINK TIMEOUT -> release all\n");
            serial_send_release(&ser, (uint16_t)(last_seq + 1));
        }

        serial_open(&ser, t);
        serial_pump(&ser);

        if (link_up && t >= next_tick_ms) {
            in_pps = in_this_sec;
            in_this_sec = 0;
            next_tick_ms = t + TICK_MS;
            publish = true; /* live counters */
            if (!quiet) {
                int nkeys = popcount_bitmap(latest.keys, HIDWAY_KEY_BITMAP_BYTES);
                fprintf(stderr,
                        "in=%4u/s ok=%-8u gaps=%-4u serial=%s replaced=%u ratedrop=%u"
                        " | mods=%02x keys=%d btn=%02x x=%ld y=%ld\n",
                        in_pps, frames_ok, seq_gaps,
                        !ser.dev ? "none" : ser.fd >= 0 ? "open" : "CLOSED", ser.replaced,
                        rate.drops, latest.mods, nkeys, latest.buttons, (long)latest.x,
                        (long)latest.y);
            }
        }
        if (!link_up)
            in_pps = in_this_sec = 0;

        if (publish || link_up != pub_link || (ser.fd >= 0) != pub_serial ||
            have_session != pub_session || session != pub_session_id || target_state(&ser) != pub_target) {
            pub_link = link_up;
            pub_serial = ser.fd >= 0;
            pub_target = target_state(&ser);
            pub_session = have_session;
            pub_session_id = session;
            publish = false;
            relay_view_t v = {
                .link_up = link_up,
                .have_session = have_session,
                .session = session,
                .frames_ok = frames_ok,
                .seq_gaps = seq_gaps,
                .pps = in_pps,
                .rate_drops = rate.drops,
                .started = started,
                .last_rx = have_session ? time(NULL) - (time_t)((t - last_pkt_ms) / 1000) : 0,
            };
            write_state_file(state_path, &v, &ser);
        }
    }

    fprintf(stderr, "hidwayd: shutting down\n");
    serial_send_release(&ser, (uint16_t)(last_seq + 1));
    if (ser.fd >= 0) {
        tcdrain(ser.fd);
        close(ser.fd);
    }
    if (state_path)
        unlink(state_path);
    close(fd);
    hidway_wipe(cry.key, sizeof cry.key);
    return 0;
}
