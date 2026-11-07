/* ping <host|ip> [count]: ICMP echo through the kernel's network stack. */
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
    if (argc < 2) {
        printf("usage: ping <host|ip> [count]\n");
        return 2;
    }
    long count = argc > 2 ? atol(argv[2]) : 4;
    uint32_t ip;
    int r = aurora_resolve(argv[1], &ip, 3000);
    if (r) {
        printf("ping: %s: %s\n", argv[1], errno == ENOENT ? "name not found" : strerror(errno));
        return 1;
    }
    char ipstr[16];
    aurora_format_ipv4(ip, ipstr, sizeof ipstr);
    printf("PING %s (%s)\n", argv[1], ipstr);
    int received = 0;
    for (long i = 1; i <= count; i++) {
        long rtt = aurora_ping(ip, (uint16_t)i, 2000);
        if (rtt >= 0) {
            printf("reply from %s: seq=%ld time=%ld ms\n", ipstr, i, rtt);
            received++;
        } else {
            printf("seq=%ld: %s\n", i, strerror(errno));
        }
        if (i < count)
            usleep(500000);
    }
    printf("--- %s: %ld sent, %d received ---\n", ipstr, count, received);
    return received ? 0 : 1;
}
