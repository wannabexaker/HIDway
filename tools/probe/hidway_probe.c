/*
 * hidway-probe - reachability and round-trip time to a hidwayd relay.
 *
 * Sends PROBE packets (sealed when a key is configured) that the relay
 * answers but never forwards to the Pico, so it can be run at any time
 * without arming anything or typing on the target. Use it to compare network
 * paths (Tailscale direct, DERP, Cloudflare, ...) and to check that both ends
 * agree on the end-to-end key.
 *
 *   hidway-probe --ini hidway.ini            target/port/key from the client config
 *   hidway-probe HOST [PORT] [--key HEX | --key-file FILE] [-n COUNT] [-i MS]
 *   hidway-probe ... --test-replay           send one sealed probe twice; a
 *                                            correct relay answers exactly once
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "hidway_crypto.h"
#include "net.h"
#include "protocol.h"

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: hidway-probe --ini hidway.ini [-n COUNT] [-i MS]\n"
            "       hidway-probe HOST [PORT] [--key HEX | --key-file FILE] [-n COUNT] [-i MS]\n");
}

static int read_key_file(const char *path, char *hex, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = fread(hex, 1, cap - 1, f);
    fclose(f);
    hex[n] = 0;
    return 1;
}

int main(int argc, char **argv)
{
    const char *host = NULL, *ini = NULL, *key_hex = NULL, *key_file = NULL;
    int port = 0, count = 50, interval_ms = 100, test_replay = 0;
    static hidway_config_t cfg;
    char keybuf[256] = {0};

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--ini") && i + 1 < argc) ini = argv[++i];
        else if (!strcmp(a, "--key") && i + 1 < argc) key_hex = argv[++i];
        else if (!strcmp(a, "--key-file") && i + 1 < argc) key_file = argv[++i];
        else if (!strcmp(a, "-n") && i + 1 < argc) count = atoi(argv[++i]);
        else if (!strcmp(a, "-i") && i + 1 < argc) interval_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--test-replay")) test_replay = 1;
        else if (a[0] == '-') { usage(); return 2; }
        else if (!host) host = a;
        else port = atoi(a);
    }
    if (ini) {
        if (!hidway_config_load(ini, &cfg)) {
            fprintf(stderr, "cannot read %s\n", ini);
            return 2;
        }
        if (!host) host = cfg.target;
        if (!port) port = cfg.port;
        if (!key_hex && !key_file && cfg.key_hex[0]) key_hex = cfg.key_hex;
    }
    if (!host) { usage(); return 2; }
    if (!port) port = 47800;
    if (count < 1) count = 1;
    if (count > 10000) count = 10000;
    if (interval_ms < 5) interval_ms = 5;

    uint8_t key[HIDWAY_KEY_LEN];
    int crypto = 0;
    if (key_file) {
        if (!read_key_file(key_file, keybuf, sizeof keybuf)) {
            fprintf(stderr, "cannot read key file %s\n", key_file);
            return 2;
        }
        key_hex = keybuf;
    }
    if (key_hex) {
        if (!hidway_key_parse(key_hex, key)) {
            fprintf(stderr, "invalid key: need exactly 64 hex characters\n");
            return 2;
        }
        crypto = 1;
    }
    hidway_wipe(keybuf, sizeof keybuf);

    if (hidway_net_open(host, port) != 0) {
        fprintf(stderr, "cannot resolve/open %s:%d\n", host, port);
        return 1;
    }
    uint32_t session = 0;
    hidway_random((uint8_t *)&session, sizeof session);
    timeBeginPeriod(1); /* 1 ms waits, otherwise RTT is quantised to ~15.6 ms */

    if (test_replay) {
        if (!crypto) {
            fprintf(stderr, "--test-replay needs a key\n");
            return 2;
        }
        hidway_input_pkt_t p;
        memset(&p, 0, sizeof p);
        p.type = HIDWAY_MSG_PROBE;
        p.session_id = session;
        p.seq = 1;
        p.client_time_us = hidway_now_us();
        uint8_t buf[HIDWAY_INPUT_PKT_LEN], env[HIDWAY_SEAL_MAX], nonce[HIDWAY_NONCE_LEN];
        size_t n = hidway_input_encode(buf, &p);
        hidway_random(nonce, sizeof nonce);
        size_t el = hidway_seal(env, sizeof env, key, nonce, hidway_wall_us(), buf, n);
        hidway_net_send(env, el);
        Sleep(50);
        hidway_net_send(env, el); /* identical bytes: a replay */
        int replies = 0;
        DWORD until = GetTickCount() + 1000;
        while ((LONG)(until - GetTickCount()) > 0) {
            uint8_t rb[HIDWAY_SEAL_MAX + 16], inner[HIDWAY_SEAL_MAX_INNER];
            int r = hidway_net_recv(rb, sizeof rb);
            if (r <= 0) { Sleep(1); continue; }
            size_t len = hidway_open(inner, sizeof inner, key, rb, (size_t)r, NULL);
            hidway_status_pkt_t s;
            if (len && hidway_status_decode(inner, len, &s) && s.session_id == session)
                replies++;
        }
        hidway_net_close();
        hidway_wipe(key, sizeof key);
        printf("replay test: sent the same sealed packet twice, got %d reply(ies) -> %s\n", replies,
               replies == 1 ? "PASS (replay rejected)" : "FAIL");
        timeEndPeriod(1);
        return replies == 1 ? 0 : 1;
    }

    printf("probing %s:%d  (%s, %d probes, %d ms apart)\n", host, port,
           crypto ? "end-to-end encrypted" : "plaintext", count, interval_ms);

    uint32_t *rtt = (uint32_t *)calloc((size_t)count, sizeof *rtt);
    int got = 0, rejected = 0, serial_open = 0, relay_encrypted = 0;
    if (!rtt)
        return 1;

    for (int i = 0; i <= count; i++) {
        if (i < count) {
            hidway_input_pkt_t p;
            memset(&p, 0, sizeof p);
            p.type = HIDWAY_MSG_PROBE;
            p.session_id = session;
            p.seq = (uint32_t)i + 1;
            p.client_time_us = hidway_now_us();
            uint8_t buf[HIDWAY_INPUT_PKT_LEN], env[HIDWAY_SEAL_MAX], nonce[HIDWAY_NONCE_LEN];
            size_t n = hidway_input_encode(buf, &p);
            if (crypto) {
                if (hidway_random(nonce, sizeof nonce) != 0)
                    continue;
                size_t el = hidway_seal(env, sizeof env, key, nonce, hidway_wall_us(), buf, n);
                if (el)
                    hidway_net_send(env, el);
            } else {
                hidway_net_send(buf, n);
            }
        }
        /* collect replies until the next probe is due (longer after the last) */
        DWORD until = GetTickCount() + (DWORD)(i < count ? interval_ms : 1000);
        while ((LONG)(until - GetTickCount()) > 0) {
            uint8_t rb[HIDWAY_SEAL_MAX + 16], inner[HIDWAY_SEAL_MAX_INNER];
            int r = hidway_net_recv(rb, sizeof rb);
            if (r <= 0) {
                Sleep(1);
                continue;
            }
            const uint8_t *pkt = rb;
            size_t len = (size_t)r;
            if (crypto) {
                len = hidway_open(inner, sizeof inner, key, rb, (size_t)r, NULL);
                if (!len) { rejected++; continue; }
                pkt = inner;
            } else if (hidway_is_sealed(rb, (size_t)r)) {
                rejected++;
                continue;
            }
            hidway_status_pkt_t s;
            if (!hidway_status_decode(pkt, len, &s) || s.session_id != session)
                continue;
            if (got < count)
                rtt[got++] = hidway_now_us() - s.client_time_us;
            serial_open = (s.flags & 0x0002) != 0;
            relay_encrypted = (s.flags & 0x0004) != 0;
        }
    }
    hidway_net_close();
    hidway_wipe(key, sizeof key);
    timeEndPeriod(1);

    double loss = 100.0 * (count - got) / count;
    printf("replies %d/%d  (loss %.1f %%)\n", got, count, loss);
    if (rejected)
        printf("ignored %d reply(ies) that did not match this end's key/mode\n", rejected);
    if (got) {
        qsort(rtt, (size_t)got, sizeof *rtt, cmp_u32);
        double sum = 0;
        for (int i = 0; i < got; i++)
            sum += rtt[i];
        printf("rtt ms   min %.1f   p50 %.1f   avg %.1f   p95 %.1f   max %.1f\n",
               rtt[0] / 1000.0, rtt[got / 2] / 1000.0, sum / got / 1000.0,
               rtt[(int)((got - 1) * 0.95)] / 1000.0, rtt[got - 1] / 1000.0);
        printf("relay: encryption %s, serial %s\n", relay_encrypted ? "on" : "off",
               serial_open ? "open" : "not open");
    } else {
        printf("no reply: relay down, wrong address/port, or the two ends disagree on the key\n");
    }
    free(rtt);
    return got ? 0 : 1;
}
