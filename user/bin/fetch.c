/* fetch <http://host[:port]/path> [file]: HTTP/1.1 GET over the kernel's TCP.
 * Prints the status line and a summary (or saves the body to a file).
 * Plain HTTP only: there is no TLS in Aurora. */
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <aurora.h>

static uint32_t crc32_user(const uint8_t *p, size_t n, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static char resp[65536 + 4096];

int main(int argc, char **argv)
{
    if (argc < 2 || strncmp(argv[1], "http://", 7) != 0) {
        printf("usage: fetch http://host[:port]/path [save-to-file]\n");
        if (argc >= 2 && !strncmp(argv[1], "https://", 8))
            printf("fetch: https is not supported (no TLS)\n");
        return 2;
    }
    char host[128], path[200];
    const char *h = argv[1] + 7;
    const char *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl == 0 || hl >= sizeof host) {
        printf("fetch: bad URL\n");
        return 2;
    }
    memcpy(host, h, hl);
    host[hl] = 0;
    snprintf(path, sizeof path, "%s", slash ? slash : "/");
    uint16_t port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = (uint16_t)atol(colon + 1);
    }
    uint32_t ip;
    int r = aurora_resolve(host, &ip, 4000);
    if (r) {
        printf("fetch: %s: %s\n", host, errno == ENOENT ? "name not found" : strerror(errno));
        return 1;
    }
    int s = aurora_socket(AURORA_SOCK_TCP);
    if (s < 0) {
        printf("fetch: %s\n", strerror(errno));
        return 1;
    }
    uint64_t t0 = aurora_uptime_ms();
    r = aurora_connect(s, ip, port, 5000);
    if (r) {
        printf("fetch: connect %s:%u: %s\n", host, port, strerror(errno));
        close(s);
        return 1;
    }
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Aurora-fetch/0.1\r\nConnection: close\r\n\r\n", path, host);
    if (aurora_send(s, req, (size_t)n) != n) {
        printf("fetch: send failed\n");
        close(s);
        return 1;
    }
    size_t total = 0;
    long got;
    while (total < sizeof resp && (got = aurora_recv(s, resp + total, sizeof resp - total, 10000)) > 0)
        total += (size_t)got;
    close(s);
    if (got < 0 && total == 0) {
        printf("fetch: receive: %s\n", strerror(errno));
        return 1;
    }
    /* Split headers and body. */
    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < total; i++)
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            hdr_end = i + 4;
            break;
        }
    if (!hdr_end) {
        printf("fetch: malformed response (%zu bytes)\n", total);
        return 1;
    }
    char status[96];
    size_t sl = 0;
    while (sl < sizeof status - 1 && resp[sl] != '\r')
        status[sl] = resp[sl], sl++;
    status[sl] = 0;
    size_t body = total - hdr_end;
    uint32_t crc = crc32_user((const uint8_t *)resp + hdr_end, body, 0);
    printf("%s\n", status);
    printf("fetch: %zu body bytes in %llu ms, crc32 %08x\n", body, (unsigned long long)(aurora_uptime_ms() - t0), crc);
    if (argc > 2) {
        int fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write(fd, resp + hdr_end, body) != (long)body) {
            printf("fetch: cannot save to %s: %s\n", argv[2], strerror(errno));
            return 1;
        }
        close(fd);
        printf("fetch: saved to %s\n", argv[2]);
    } else {
        size_t show = body < 300 ? body : 300;
        write(1, resp + hdr_end, show);
        if (show && resp[hdr_end + show - 1] != '\n')
            printf("\n");
        if (show < body)
            printf("... (%zu more bytes)\n", body - show);
    }
    /* "HTTP/1.x 200 ..." */
    return strncmp(status, "HTTP/1.", 7) == 0 && strncmp(status + 8, " 200", 4) == 0 ? 0 : 1;
}
