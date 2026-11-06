/* fsstress ID ROUNDS: rewrites four files in /home/fsID for ROUNDS rounds,
 * reading each back and comparing every byte and its CRC-32. Several copies
 * run in parallel on different CPUs in the SMP stress test. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <aurora.h>

static uint8_t wbuf[4096], rbuf[4096];

static size_t fill(long id, long round, int k)
{
    uint32_t x = (uint32_t)(id * 7919 + round * 104729 + k * 1299709) | 1;
    size_t n = 1000 + (size_t)((round * 7 + k * 13 + id * 3) % 3000);
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        wbuf[i] = (uint8_t)x;
    }
    return n;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: fsstress ID ROUNDS\n");
        return 2;
    }
    long id = atol(argv[1]), rounds = atol(argv[2]);
    char dir[32], path[64];
    snprintf(dir, sizeof dir, "/home/fs%ld", id);
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        printf("fsstress %ld: mkdir %s failed: %s\n", id, dir, strerror(errno));
        return 1;
    }
    long errors = 0, bytes = 0;
    uint32_t total = 0;
    for (long r = 0; r < rounds && errors == 0; r++) {
        for (int k = 0; k < 4; k++) {
            snprintf(path, sizeof path, "%s/f%d", dir, k);
            size_t n = fill(id, r, k);
            uint32_t want = crc32_update(0, wbuf, n);
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0 || write(fd, wbuf, n) != (ssize_t)n) {
                printf("fsstress %ld: write %s failed: %s\n", id, path, strerror(errno));
                errors++;
                if (fd >= 0)
                    close(fd);
                break;
            }
            close(fd);
            fd = open(path, O_RDONLY);
            ssize_t got = fd >= 0 ? read(fd, rbuf, sizeof rbuf) : -1;
            if (fd >= 0)
                close(fd);
            uint32_t have = got > 0 ? crc32_update(0, rbuf, (size_t)got) : 0;
            if (got != (ssize_t)n || have != want || memcmp(rbuf, wbuf, n)) {
                printf("fsstress %ld: MISMATCH %s round %ld: %ld/%zu bytes crc %08x want %08x\n", id, path, r,
                       (long)got, n, have, want);
                errors++;
                break;
            }
            bytes += (long)n;
            total = crc32_update(total, &want, 4);
        }
    }
    printf("fsstress %ld: %ld rounds, %ld bytes verified, %ld errors, digest %08x\n", id, rounds, bytes, errors,
           total);
    return errors ? 1 : 0;
}
