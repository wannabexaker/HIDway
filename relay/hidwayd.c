/*
 * hidwayd - HIDway relay for the Raspberry Pi.
 *
 * Receives full-state input packets from the client over UDP (through
 * Tailscale), keeps only the latest, and (later) forwards it to the Pico over
 * a serial link. It echoes a status packet back to the client for RTT and
 * loss measurement.
 *
 * Step 1 (this build): no serial yet. It validates and accounts for packets,
 * prints a once-per-second summary, and echoes status. The serial output and
 * the fail-safe "release all on link loss" are wired at the marked points.
 *
 * Principles: accept only from the allowed peer; never queue (keep the latest
 * state only); treat loss of the client as "release everything".
 */
#define _GNU_SOURCE /* cfmakeraw, CRTSCTS, getopt_long, clock_gettime */
#include <arpa/inet.h>
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
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "serial.h"

#define LINK_TIMEOUT_MS 250
#define STATS_PERIOD_MS 1000

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

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

static int serial_open(const char *dev, int baud)
{
    speed_t sp = baud_const(baud);
    if (!sp) {
        fprintf(stderr, "unsupported baud: %d\n", baud);
        return -1;
    }
    int fd = open(dev, O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        perror("open serial");
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetospeed(&t, sp);
        cfsetispeed(&t, sp);
        t.c_cflag |= CLOCAL | CREAD;
        t.c_cflag &= (tcflag_t)~CRTSCTS;
        tcsetattr(fd, TCSANOW, &t);
    }
    return fd;
}

/* Write one complete frame. Serial is fast (one frame ~0.5 ms at 921600) and
 * we write at most at the incoming rate, so this never backs up into a queue. */
static void serial_write_frame(int fd, const hidway_serial_state_t *s)
{
    if (fd < 0)
        return;
    uint8_t f[HIDWAY_SER_FRAME_MAX];
    size_t n = hidway_serial_build(s, f, sizeof f);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, f + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            break; /* drop; the next frame carries full state again */
        }
        off += (size_t)w;
    }
}

static void serial_send_state(int fd, const hidway_input_pkt_t *p)
{
    hidway_serial_state_t s;
    s.type = HIDWAY_SER_STATE;
    s.seq = (uint16_t)p->seq;
    s.mods = p->mods;
    memcpy(s.keys, p->keys, HIDWAY_KEY_BITMAP_BYTES);
    s.buttons = p->buttons;
    s.x = p->x;
    s.y = p->y;
    s.wheel = p->wheel;
    s.pan = p->pan;
    serial_write_frame(fd, &s);
}

static void serial_send_release(int fd, uint16_t seq)
{
    hidway_serial_state_t s = {0};
    s.type = HIDWAY_SER_RELEASE;
    s.seq = seq;
    serial_write_frame(fd, &s);
}

static int popcount_bitmap(const uint8_t *bm, size_t n)
{
    int c = 0;
    for (size_t i = 0; i < n; i++)
        for (uint8_t b = bm[i]; b; b &= (uint8_t)(b - 1))
            c++;
    return c;
}

int main(int argc, char **argv)
{
    const char *bind_addr = "0.0.0.0";
    const char *allow_addr = NULL; /* if set, only accept this source IP */
    const char *serial_dev = NULL; /* if set, forward to the Pico here */
    int baud = 921600;
    int port = 47800;
    int quiet = 0;

    static const struct option opts[] = {
        {"bind", required_argument, 0, 'b'},
        {"allow", required_argument, 0, 'a'},
        {"port", required_argument, 0, 'p'},
        {"serial", required_argument, 0, 's'},
        {"baud", required_argument, 0, 'B'},
        {"quiet", no_argument, 0, 'q'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "b:a:p:s:B:q", opts, NULL)) != -1) {
        switch (c) {
        case 'b': bind_addr = optarg; break;
        case 'a': allow_addr = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 's': serial_dev = optarg; break;
        case 'B': baud = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default:
            fprintf(stderr, "usage: %s [--bind IP] [--allow IP] [--port N]"
                            " [--serial DEV] [--baud N] [--quiet]\n", argv[0]);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
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

    int serial_fd = -1;
    if (serial_dev) {
        serial_fd = serial_open(serial_dev, baud);
        if (serial_fd < 0)
            fprintf(stderr, "hidwayd: continuing without serial (dump/echo only)\n");
    }

    fprintf(stderr, "hidwayd: listening on %s:%d%s%s | serial: %s\n",
            bind_addr, port, allow_addr ? ", allow=" : "", allow_addr ? allow_addr : "",
            serial_fd >= 0 ? serial_dev : "none (dump/echo mode)");

    /* Per-session accounting. */
    uint32_t session = 0;
    uint32_t last_seq = 0;
    bool have_session = false;
    uint32_t frames_ok = 0;
    uint16_t seq_gaps = 0;
    hidway_input_pkt_t latest = {0};

    bool link_up = false;
    uint64_t last_pkt_ms = 0;
    uint64_t next_stats_ms = now_ms() + STATS_PERIOD_MS;
    uint32_t in_this_sec = 0, in_pps = 0;

    while (running) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 50);
        uint64_t t = now_ms();

        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint8_t buf[128];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof from;
            ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
            if (n < 0) {
                if (errno != EINTR) perror("recvfrom");
            } else if (!allow_addr || from.sin_addr.s_addr == allow_in.s_addr) {
                hidway_input_pkt_t p;
                if (hidway_input_decode(buf, (size_t)n, &p) && p.type == HIDWAY_MSG_PROBE) {
                    /* Reachability ping: reply, but do not touch link state,
                     * the latest input, the serial link or the counters. */
                    hidway_status_pkt_t st = {0};
                    st.session_id = p.session_id;
                    st.seq = last_seq;
                    st.client_time_us = p.client_time_us;
                    st.frames_ok = frames_ok;
                    st.seq_gaps = seq_gaps;
                    st.flags = (serial_fd >= 0) ? 0x0002 : 0;
                    uint8_t sb[HIDWAY_STATUS_PKT_LEN];
                    size_t sl = hidway_status_encode(sb, &st);
                    sendto(fd, sb, sl, 0, (struct sockaddr *)&from, fromlen);
                } else if (hidway_input_decode(buf, (size_t)n, &p)) {
                    if (!have_session || p.session_id != session) {
                        session = p.session_id;
                        have_session = true;
                        last_seq = p.seq;   /* baseline; no gap for the first */
                        frames_ok = 0;
                        seq_gaps = 0;
                        fprintf(stderr, "hidwayd: new session %08x\n", session);
                    } else if (p.seq > last_seq) {
                        seq_gaps = (uint16_t)(seq_gaps + (p.seq - last_seq - 1));
                        last_seq = p.seq;
                    } else {
                        /* stale or duplicate: cumulative state makes this safe
                         * to drop entirely */
                        goto after_apply;
                    }

                    frames_ok++;
                    in_this_sec++;
                    latest = p;
                    link_up = true;
                    last_pkt_ms = t;

                    /* Forward the latest full state to the Pico. */
                    serial_send_state(serial_fd, &latest);

                    /* Echo status back for RTT/loss measurement. */
                    hidway_status_pkt_t st = {0};
                    st.session_id = session;
                    st.seq = last_seq;
                    st.client_time_us = p.client_time_us;
                    st.frames_ok = frames_ok;
                    st.seq_gaps = seq_gaps;
                    st.flags = (serial_fd >= 0) ? 0x0002 : 0; /* bit1: relay serial open */
                    st.leds = 0;
                    uint8_t sb[HIDWAY_STATUS_PKT_LEN];
                    size_t sl = hidway_status_encode(sb, &st);
                    sendto(fd, sb, sl, 0, (struct sockaddr *)&from, fromlen);
                }
            }
        }
    after_apply:

        if (link_up && t - last_pkt_ms > LINK_TIMEOUT_MS) {
            link_up = false;
            fprintf(stderr, "hidwayd: LINK TIMEOUT -> release all\n");
            serial_send_release(serial_fd, (uint16_t)(last_seq + 1));
        }

        if (t >= next_stats_ms) {
            in_pps = in_this_sec;
            in_this_sec = 0;
            next_stats_ms += STATS_PERIOD_MS;
            if (!quiet && have_session) {
                int nkeys = popcount_bitmap(latest.keys, HIDWAY_KEY_BITMAP_BYTES);
                fprintf(stderr,
                        "in=%4u/s ok=%-8u gaps=%-4u link=%s | mods=%02x keys=%d btn=%02x x=%ld y=%ld\n",
                        in_pps, frames_ok, seq_gaps, link_up ? "up" : "DOWN",
                        latest.mods, nkeys, latest.buttons, (long)latest.x, (long)latest.y);
            }
        }
    }

    fprintf(stderr, "hidwayd: shutting down\n");
    if (serial_fd >= 0) {
        serial_send_release(serial_fd, (uint16_t)(last_seq + 1));
        close(serial_fd);
    }
    close(fd);
    return 0;
}
