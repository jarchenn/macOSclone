/* ping <host|ip> [count]: ICMP echo through the kernel's network stack. */
#include "aurora.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: ping <host|ip> [count]\n");
        return 2;
    }
    long count = argc > 2 ? atol(argv[2]) : 4;
    uint32_t ip;
    int r = resolve(argv[1], &ip, 3000);
    if (r) {
        printf("ping: %s: %s\n", argv[1], strerror(r));
        return 1;
    }
    char ipstr[16];
    format_ipv4(ip, ipstr, sizeof ipstr);
    printf("PING %s (%s)\n", argv[1], ipstr);
    int received = 0;
    for (long i = 1; i <= count; i++) {
        long rtt = ping(ip, (uint16_t)i, 2000);
        if (rtt >= 0) {
            printf("reply from %s: seq=%ld time=%ld ms\n", ipstr, i, rtt);
            received++;
        } else {
            printf("seq=%ld: %s\n", i, strerror(rtt));
        }
        if (i < count)
            sleep_ms(500);
    }
    printf("--- %s: %ld sent, %d received ---\n", ipstr, count, received);
    return received ? 0 : 1;
}
