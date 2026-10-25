/*
 * Fail-safe tests for hidwayd, run against the real binary. A fake client
 * sends input over UDP; a fake Pico (a pseudo-terminal used as the serial
 * device) records every frame the relay writes, with its arrival time. Every
 * way the input stream can stop must end in a RELEASE frame on the serial
 * line. The key used is F24, harmless if it ever reached a real target.
 *
 *   make -C relay failsafe            (Linux: builds hidwayd and this test)
 *
 * Not part of `make check`: these are timing tests, and `make check` gates
 * every deployment on the Pi. A relay killed outright (SIGKILL, power loss)
 * cannot send anything; the Pico's own link timeout covers that case.
 */
#define _GNU_SOURCE /* posix_openpt, ptsname, mkdtemp */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "serial.h"

#define PORT            47899
#define KEY             0x73 /* F24 */
#define SEND_EVERY_MS   10   /* the fake client's packet rate: 100 Hz */
#define LINK_TIMEOUT_MS 250  /* hidwayd's */

static int pass, fail;
static const char *relay_bin;
static char dir[] = "/tmp/hidway-failsafe-XXXXXX";
static char dev_link[64], log_path[64];
static pid_t relay = -1;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void check(const char *name, bool ok)
{
    printf("  %s  %s\n", ok ? "ok  " : "FAIL", name);
    if (ok)
        pass++;
    else
        fail++;
}

/* ------------------------------------------------------------- fake Pico */

/* A pty whose slave is reached through a fixed path, so it can be unplugged
 * (master closed) and plugged in again (a new pty behind the same path). The
 * test keeps its own slave descriptor open, so frames written just before the
 * relay closes the port are still readable. */
typedef struct {
    uint64_t t;
    uint8_t type;
    bool key;
} frame_t;

static frame_t frames[4096];
static int nframes;
static int pico = -1, pico_slave = -1;
static hidway_deframer_t df;

static void pico_plug(void)
{
    pico = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (pico < 0 || grantpt(pico) || unlockpt(pico)) {
        perror("pty");
        exit(2);
    }
    pico_slave = open(ptsname(pico), O_RDWR | O_NOCTTY | O_CLOEXEC);
    unlink(dev_link);
    if (pico_slave < 0 || symlink(ptsname(pico), dev_link)) {
        perror("pty slave");
        exit(2);
    }
    hidway_deframer_reset(&df);
}

static void pico_unplug(void)
{
    unlink(dev_link);
    close(pico_slave);
    close(pico);
    pico = pico_slave = -1;
}

static void pico_read(void)
{
    uint8_t b[256];
    ssize_t n;
    while (pico >= 0 && (n = read(pico, b, sizeof b)) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            hidway_serial_state_t s;
            if (hidway_deframer_push(&df, b[i], &s) && nframes < (int)(sizeof frames / sizeof frames[0]))
                frames[nframes++] = (frame_t){now_ms(), s.type, hidway_bitmap_test(s.keys, KEY)};
        }
    }
}

static int first(uint8_t type, int from)
{
    for (int i = from; i < nframes; i++)
        if (frames[i].type == type)
            return i;
    return -1;
}

static bool any_state(bool key)
{
    for (int i = 0; i < nframes; i++)
        if (frames[i].type == HIDWAY_SER_STATE && frames[i].key == key)
            return true;
    return false;
}

/* ----------------------------------------------------------- fake client */

static int cli = -1, rogue = -1;
static struct sockaddr_in relay_addr;
static uint32_t session, seq;
static uint64_t last_send_ms;

static int udp_from(const char *ip)
{
    struct sockaddr_in a = {.sin_family = AF_INET};
    inet_pton(AF_INET, ip, &a.sin_addr);
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a)) {
        perror(ip);
        exit(2);
    }
    return s;
}

static void send_input(int sock, uint8_t type, bool key, uint32_t sq)
{
    hidway_input_pkt_t p = {.type = type, .session_id = session, .seq = sq};
    p.client_time_us = (uint32_t)(now_ms() * 1000u);
    if (key)
        hidway_bitmap_set(p.keys, KEY);
    uint8_t b[HIDWAY_INPUT_PKT_LEN];
    size_t n = hidway_input_encode(b, &p);
    sendto(sock, b, n, 0, (struct sockaddr *)&relay_addr, sizeof relay_addr);
    last_send_ms = now_ms();
}

/* For `ms`: when `sending`, send the state (F24 held or not) every 10 ms
 * from `sock`; record every frame the relay writes; discard status replies. */
static void run(int ms, int sock, bool sending, bool key)
{
    uint64_t end = now_ms() + (uint64_t)ms, next = now_ms();
    uint8_t junk[64];
    for (uint64_t t; (t = now_ms()) < end;) {
        if (sending && t >= next) {
            send_input(sock, HIDWAY_MSG_STATE, key, ++seq);
            next = t + SEND_EVERY_MS;
        }
        struct pollfd pf = {.fd = pico, .events = POLLIN};
        poll(&pf, pico >= 0 ? 1 : 0, 1);
        pico_read();
        while (recv(cli, junk, sizeof junk, MSG_DONTWAIT) > 0) {}
    }
}

/* ----------------------------------------------------------------- relay */

static void relay_start(void)
{
    char port[8];
    snprintf(port, sizeof port, "%d", PORT);
    relay = fork();
    if (relay == 0) {
        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        dup2(fd, 1);
        dup2(fd, 2);
        execl(relay_bin, relay_bin, "--bind", "127.0.0.1", "--allow", "127.0.0.1", "--port", port,
              "--serial", dev_link, (char *)NULL);
        _exit(127);
    }
}

int main(int argc, char **argv)
{
    relay_bin = argc > 1 ? argv[1] : "./hidwayd";
    signal(SIGPIPE, SIG_IGN);
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 2;
    }
    snprintf(dev_link, sizeof dev_link, "%s/serial", dir);
    snprintf(log_path, sizeof log_path, "%s/hidwayd.log", dir);
    relay_addr = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(PORT)};
    inet_pton(AF_INET, "127.0.0.1", &relay_addr.sin_addr);
    cli = udp_from("127.0.0.1");
    rogue = udp_from("127.0.0.2");
    session = 0x48000000u | (uint32_t)getpid();

    printf("hidwayd fail-safe tests (%s)\n", relay_bin);
    pico_plug();
    relay_start();

    nframes = 0;
    run(500, cli, false, false);
    check("a new serial connection starts with RELEASE", nframes > 0 && frames[0].type == HIDWAY_SER_RELEASE);

    nframes = 0;
    uint64_t t0 = now_ms();
    run(300, cli, true, true);
    int i = -1;
    for (int k = 0; k < nframes && i < 0; k++)
        if (frames[k].type == HIDWAY_SER_STATE && frames[k].key)
            i = k;
    check("a held key reaches the Pico", i >= 0);
    printf("        first frame %llu ms after the first packet\n", i >= 0 ? (unsigned long long)(frames[i].t - t0) : 0ull);
    check("  within 50 ms", i >= 0 && frames[i].t - t0 <= 50);

    /* The client stops (crash, network loss, Tailscale down). */
    nframes = 0;
    run(800, cli, false, false);
    i = first(HIDWAY_SER_RELEASE, 0);
    long long d = i >= 0 ? (long long)(frames[i].t - last_send_ms) : -1;
    check("client gone with a key held -> RELEASE", i >= 0);
    printf("        RELEASE %lld ms after the last packet (link timeout %d ms)\n", d, LINK_TIMEOUT_MS);
    check("  after the link timeout and within 400 ms", d >= LINK_TIMEOUT_MS && d <= 400);
    check("  nothing held after it", !any_state(true));

    /* The client releases on purpose (disarm, panic key, focus lost). */
    run(200, cli, true, true);
    nframes = 0;
    uint64_t tr = now_ms();
    send_input(cli, HIDWAY_MSG_RELEASE, false, ++seq);
    run(100, cli, false, false);
    i = first(HIDWAY_SER_RELEASE, 0);
    check("RELEASE from the client is forwarded", i >= 0);
    check("  within 50 ms", i >= 0 && frames[i].t - tr <= 50);
    run(400, cli, false, false); /* let the link time out */

    /* A packet delayed on the network arrives after newer ones. */
    run(100, cli, true, true);
    nframes = 0;
    send_input(cli, HIDWAY_MSG_STATE, false, seq - 5);
    run(100, cli, false, false);
    check("an older packet never overrides a newer one", !any_state(false) && first(HIDWAY_SER_RELEASE, 0) < 0);
    run(400, cli, false, false);

    /* Someone else on the network. */
    nframes = 0;
    run(300, rogue, true, true);
    check("input from any other address is ignored", !any_state(true));

    /* The serial line drops (wire pulled, adapter unplugged) with a key held. */
    session++;
    run(200, cli, true, true);
    pico_unplug();
    run(400, cli, true, true);
    pico_plug();
    nframes = 0;
    uint64_t tp = now_ms();
    run(1800, cli, true, true);
    check("after the serial line returns, the first frame is RELEASE", nframes > 0 && frames[0].type == HIDWAY_SER_RELEASE);
    printf("        reconnected %llu ms after the line returned\n", nframes > 0 ? (unsigned long long)(frames[0].t - tp) : 0ull);
    check("  within 1.5 s", nframes > 0 && frames[0].t - tp <= 1500);
    check("  then the key still held (latest state)", any_state(true));

    /* The relay is stopped (update, shutdown) with a key held. */
    run(200, cli, true, true);
    nframes = 0;
    uint64_t tk = now_ms();
    kill(relay, SIGTERM);
    run(300, cli, true, true);
    int status = 0;
    waitpid(relay, &status, 0);
    i = first(HIDWAY_SER_RELEASE, 0);
    check("relay stopped with a key held -> RELEASE", i >= 0);
    check("  within 100 ms", i >= 0 && frames[i].t - tk <= 100);
    check("  the relay exits cleanly", WIFEXITED(status) && WEXITSTATUS(status) == 0);

    pico_unplug();
    printf("hidwayd fail-safe: %d passed, %d failed\n", pass, fail);
    if (fail) {
        printf("relay log (%s):\n", log_path);
        FILE *f = fopen(log_path, "r");
        for (char line[256]; f && fgets(line, sizeof line, f);)
            printf("    %s", line);
        if (f)
            fclose(f);
    } else {
        unlink(log_path);
        rmdir(dir);
    }
    return fail ? 1 : 0;
}
