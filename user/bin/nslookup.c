/* nslookup <name> [server[:port]]: DNS A lookup through the kernel
 * resolver, using the DHCP-provided server or the one given. */
#include "aurora.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: nslookup <name> [server[:port]]\n");
        return 2;
    }
    uint32_t ip = 0, server = 0;
    uint16_t port = 53;
    if (argc > 2) {
        char host[32];
        snprintf(host, sizeof host, "%s", argv[2]);
        char *colon = strchr(host, ':');
        if (colon) {
            *colon = 0;
            port = (uint16_t)atol(colon + 1);
        }
        if (!parse_ipv4(host, &server)) {
            printf("nslookup: bad server address %s\n", host);
            return 2;
        }
    }
    int r = server ? resolve_via(argv[1], &ip, 4000, server, port) : resolve(argv[1], &ip, 4000);
    if (r) {
        printf("nslookup: %s: %s\n", argv[1], strerror(r));
        return 1;
    }
    char s[16];
    format_ipv4(ip, s, sizeof s);
    printf("%s has address %s\n", argv[1], s);
    return 0;
}
