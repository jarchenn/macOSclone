/* tcpecho <ip> <port> <bytes>: sends a generated payload to a TCP echo
 * server and verifies the echoed bytes. Exercises the send window, the
 * receive window and retransmission. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <aurora.h>

static uint8_t out[48 * 1024], in[48 * 1024];

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: tcpecho <ip> <port> <bytes>\n");
        return 2;
    }
    uint32_t ip;
    if (!aurora_parse_ipv4(argv[1], &ip)) {
        printf("tcpecho: bad address\n");
        return 2;
    }
    uint16_t port = (uint16_t)atol(argv[2]);
    size_t n = (size_t)atol(argv[3]);
    if (n > sizeof out)
        n = sizeof out;
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(i * 7 + (i >> 8));
    int s = aurora_socket(AURORA_SOCK_TCP);
    int r = s < 0 ? s : aurora_connect(s, ip, port, 5000);
    if (r) {
        printf("tcpecho: connect: %s\n", strerror(errno));
        return 1;
    }
    /* Alternate sending and draining so neither side's window stalls. */
    size_t sent = 0, got = 0;
    while (got < n) {
        if (sent < n) {
            size_t chunk = n - sent < 4096 ? n - sent : 4096;
            long w = aurora_send(s, out + sent, chunk);
            if (w < 0) {
                printf("tcpecho: send: %s\n", strerror(errno));
                return 1;
            }
            sent += (size_t)w;
        }
        long rr = aurora_recv(s, in + got, n - got, sent < n ? 50 : 10000);
        if (rr < 0 && errno == ETIMEDOUT && sent < n)
            continue;
        if (rr <= 0) {
            printf("tcpecho: receive after %zu bytes: %s\n", got, rr == 0 ? "connection closed" : strerror(errno));
            return 1;
        }
        got += (size_t)rr;
    }
    close(s);
    int bad = memcmp(in, out, n);
    printf("tcpecho: %zu bytes echoed, %s\n", got, bad ? "MISMATCH" : "identical");
    return bad ? 1 : 0;
}
