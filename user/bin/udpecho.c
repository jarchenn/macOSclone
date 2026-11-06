/* udpecho <ip> <port> <message> [count]: sends datagrams to a UDP echo
 * server and checks the replies. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <aurora.h>

int main(int argc, char **argv)
{
    if (argc < 4) {
        printf("usage: udpecho <ip> <port> <message> [count]\n");
        return 2;
    }
    uint32_t ip;
    if (!aurora_parse_ipv4(argv[1], &ip)) {
        printf("udpecho: bad address\n");
        return 2;
    }
    uint16_t port = (uint16_t)atol(argv[2]);
    long count = argc > 4 ? atol(argv[4]) : 3;
    int s = aurora_socket(AURORA_SOCK_UDP);
    if (s < 0) {
        printf("udpecho: %s\n", strerror(errno));
        return 1;
    }
    int ok = 0;
    for (long i = 1; i <= count; i++) {
        char msg[200];
        int n = snprintf(msg, sizeof msg, "%s #%ld", argv[3], i);
        long r = aurora_sendto(s, msg, (size_t)n, ip, port);
        if (r < 0) {
            printf("udpecho: send: %s\n", strerror(errno));
            break;
        }
        char reply[256];
        struct aurora_sockaddr from;
        long got = aurora_recvfrom(s, reply, sizeof reply - 1, &from, 2000);
        if (got < 0) {
            printf("udpecho: #%ld: %s\n", i, strerror(errno));
            continue;
        }
        reply[got] = 0;
        bool match = got == n && memcmp(reply, msg, (size_t)n) == 0;
        printf("udpecho: reply %s from port %u\n", reply, from.port);
        ok += match;
    }
    close(s);
    printf("udpecho: %d/%ld replies matched\n", ok, count);
    return ok == count ? 0 : 1;
}
