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
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"

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
    int port = 47800;
    int quiet = 0;

    static const struct option opts[] = {
        {"bind", required_argument, 0, 'b'},
        {"allow", required_argument, 0, 'a'},
        {"port", required_argument, 0, 'p'},
        {"quiet", no_argument, 0, 'q'},
        {0, 0, 0, 0},
    };
    int c;
    while ((c = getopt_long(argc, argv, "b:a:p:q", opts, NULL)) != -1) {
        switch (c) {
        case 'b': bind_addr = optarg; break;
        case 'a': allow_addr = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'q': quiet = 1; break;
        default:
            fprintf(stderr, "usage: %s [--bind IP] [--allow IP] [--port N] [--quiet]\n", argv[0]);
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

    fprintf(stderr, "hidwayd: listening on %s:%d%s%s (no serial yet - dump/echo mode)\n",
            bind_addr, port, allow_addr ? ", allow=" : "", allow_addr ? allow_addr : "");

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
                if (hidway_input_decode(buf, (size_t)n, &p)) {
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

                    /* >>> serial output goes here: encode `latest` as a UART
                     *     frame and write the newest one (drop any queued). */

                    /* Echo status back for RTT/loss measurement. */
                    hidway_status_pkt_t st = {0};
                    st.session_id = session;
                    st.seq = last_seq;
                    st.client_time_us = p.client_time_us;
                    st.frames_ok = frames_ok;
                    st.seq_gaps = seq_gaps;
                    st.flags = 0;
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
            /* >>> serial release goes here: send a RELEASE frame to the Pico. */
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
    close(fd);
    return 0;
}
