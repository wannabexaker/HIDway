#include "net.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")

static SOCKET sock = INVALID_SOCKET;
static LARGE_INTEGER perf_freq;

int hidway_net_open(const char *host, int port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return -1;

    QueryPerformanceFrequency(&perf_freq);

    char portstr[16];
    snprintf(portstr, sizeof portstr, "%d", port);

    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC; /* allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -2;

    int rc = -3;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET)
            continue;
        /* connect() on a UDP socket fixes the peer: send/recv only talk to it
         * and ICMP port-unreachable surfaces as an error instead of silently. */
        if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            u_long nb = 1;
            ioctlsocket(sock, FIONBIO, &nb);
            rc = 0;
            break;
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return rc;
}

void hidway_net_close(void)
{
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        sock = INVALID_SOCKET;
    }
    WSACleanup();
}

int hidway_net_send(const uint8_t *buf, size_t len)
{
    if (sock == INVALID_SOCKET)
        return -1;
    int n = send(sock, (const char *)buf, (int)len, 0);
    return n;
}

int hidway_net_recv(uint8_t *buf, size_t cap)
{
    if (sock == INVALID_SOCKET)
        return -1;
    int n = recv(sock, (char *)buf, (int)cap, 0);
    if (n == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK)
            return 0;
        return -1;
    }
    return n;
}

uint32_t hidway_now_us(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (perf_freq.QuadPart == 0)
        return 0;
    /* microseconds, truncated to 32 bits */
    return (uint32_t)((t.QuadPart * 1000000ULL) / (unsigned long long)perf_freq.QuadPart);
}
