#ifndef HIDWAY_CLIENT_NET_H
#define HIDWAY_CLIENT_NET_H

#include <stddef.h>
#include <stdint.h>

/* Connected UDP socket to the relay. All functions are non-blocking.
 * Returns 0 on success, non-zero on error. */
int hidway_net_open(const char *host, int port);
void hidway_net_close(void);

/* -1 on error, otherwise bytes sent. */
int hidway_net_send(const uint8_t *buf, size_t len);

/* Reads one pending datagram from the relay. Returns bytes read, 0 if none
 * pending, -1 on error. */
int hidway_net_recv(uint8_t *buf, size_t cap);

/* Monotonic microsecond clock (wraps at 2^32, fine for RTT differences). */
uint32_t hidway_now_us(void);

/* Wall-clock microseconds since 1970 that never goes backwards in this
 * process (startup wall time + monotonic elapsed time). Used as the sealed
 * packet timestamp the relay checks for replays. */
uint64_t hidway_wall_us(void);

/* Fill `p` with cryptographically secure random bytes. Returns 0 on success. */
int hidway_random(uint8_t *p, size_t n);

#endif /* HIDWAY_CLIENT_NET_H */
