/* posixtest: 0.6 acceptance check for an ordinary portable C program -
 * printf (integers, strings, floating point), malloc/free, stdio and POSIX
 * file I/O, mmap/mprotect, and pthreads (create/join, mutexes, condition
 * variables, thread-local storage). Prints one line per check and
 * "posixtest: N/N passed". */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

static int passed, total;

static void check(const char *what, int ok)
{
    total++;
    passed += ok != 0;
    printf("posixtest: %s %s\n", ok ? "ok  " : "FAIL", what);
}

/* ---- printf / strtod -------------------------------------------------------- */

static void test_printf(void)
{
    char b[128];
    snprintf(b, sizeof b, "%d %5s|%-4x|%lld %c", -42, "ab", 255, 1234567890123ll, 'z');
    check("printf integers and strings", !strcmp(b, "-42    ab|ff  |1234567890123 z"));
    snprintf(b, sizeof b, "%.3f %e %g %g", 3.14159265, 12345.678, 0.0001, 1e20);
    check("printf floating point", !strcmp(b, "3.142 1.234568e+04 0.0001 1e+20"));
    double d = strtod("2.5e-3", NULL);
    check("strtod", fabs(d - 0.0025) < 1e-15);
    check("libm sqrt/sin/exp", fabs(sqrt(2.0) - 1.41421356237) < 1e-10 && fabs(sin(0.0)) < 1e-15 &&
                                   fabs(exp(1.0) - 2.718281828459045) < 1e-12);
}

/* ---- malloc ------------------------------------------------------------------ */

static void test_malloc(void)
{
    void *blocks[512];
    int ok = 1;
    for (int i = 0; i < 512; i++) {
        size_t n = (size_t)(i * 37 % 4000) + 1;
        blocks[i] = malloc(n);
        if (!blocks[i]) {
            ok = 0;
            break;
        }
        memset(blocks[i], i & 0xff, n);
    }
    for (int i = 0; ok && i < 512; i++) {
        size_t n = (size_t)(i * 37 % 4000) + 1;
        for (size_t k = 0; k < n; k++)
            if (((unsigned char *)blocks[i])[k] != (i & 0xff))
                ok = 0;
    }
    for (int i = 0; i < 512; i++)
        free(blocks[i]);
    void *big = malloc(8u << 20); /* 8 MiB: needs the break to grow */
    if (big)
        memset(big, 0x5a, 8u << 20);
    check("malloc/free 512 blocks + 8 MiB", ok && big != NULL);
    free(big);
    char *r = realloc(NULL, 10);
    strcpy(r, "123456789");
    r = realloc(r, 100000);
    check("realloc keeps contents", r && !strcmp(r, "123456789"));
    free(r);
}

/* ---- files ------------------------------------------------------------------- */

static void test_files(void)
{
    const char *path = "/tmp/posixtest.dat";
    FILE *f = fopen(path, "w+");
    int ok = f != NULL;
    for (int i = 0; ok && i < 1000; i++)
        ok = fprintf(f, "line %d\n", i) > 0;
    ok = ok && fflush(f) == 0;
    long size = ftell(f);
    rewind(f);
    char line[64];
    int n = 0;
    while (ok && fgets(line, sizeof line, f))
        n++;
    fseek(f, 5, SEEK_SET);
    int c = fgetc(f);
    fclose(f);
    check("stdio write/read/seek", ok && n == 1000 && c == '0' && size == 8890);
    struct stat st;
    check("stat size and mode", stat(path, &st) == 0 && st.st_size == 8890 && S_ISREG(st.st_mode));
    int fd = open(path, O_RDWR);
    ok = fd >= 0 && ftruncate(fd, 100) == 0 && lseek(fd, 0, SEEK_END) == 100;
    ok = ok && pwrite(fd, "XY", 2, 10) == 2;
    char b[4] = { 0 };
    ok = ok && pread(fd, b, 2, 10) == 2 && b[0] == 'X' && b[1] == 'Y';
    close(fd);
    check("open/ftruncate/lseek/pread/pwrite", ok);
    errno = 0;
    check("open missing file sets ENOENT", open("/tmp/does-not-exist", O_RDONLY) < 0 && errno == ENOENT);
    check("unlink", unlink(path) == 0 && stat(path, &st) < 0 && errno == ENOENT);
}

/* ---- memory mappings -------------------------------------------------------- */

static void test_mmap(void)
{
    size_t len = 64 * 4096;
    uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = p != MAP_FAILED;
    for (size_t i = 0; ok && i < len; i += 4096)
        ok = p[i] == 0; /* demand-zero */
    for (size_t i = 0; ok && i < len; i++)
        p[i] = (uint8_t)i;
    ok = ok && p[len - 1] == (uint8_t)(len - 1);
    ok = ok && mprotect(p, len, PROT_READ) == 0 && p[100] == 100;
    ok = ok && munmap(p, len) == 0;
    check("mmap/mprotect/munmap anonymous", ok);
    errno = 0;
    check("mmap MAP_SHARED is refused (ENOTSUP)",
          mmap(NULL, 4096, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == ENOTSUP);
}

/* ---- threads ---------------------------------------------------------------- */

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static long counter;
static int queue[64], qhead, qtail;
static _Thread_local int tls_value = 7;
static _Thread_local char tls_buf[64];

static void *adder(void *arg)
{
    long id = (long)arg;
    tls_value = (int)id * 100;
    snprintf(tls_buf, sizeof tls_buf, "thread %ld", id);
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    errno = (int)id + 1000; /* errno is per thread */
    sched_yield();
    char want[64];
    snprintf(want, sizeof want, "thread %ld", id);
    int ok = tls_value == (int)id * 100 && !strcmp(tls_buf, want) && errno == (int)id + 1000;
    return (void *)(long)ok;
}

static void *producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 200; i++) {
        pthread_mutex_lock(&lock);
        while ((qtail + 1) % 64 == qhead)
            pthread_cond_wait(&cond, &lock);
        queue[qtail] = i;
        qtail = (qtail + 1) % 64;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t t[8];
    int created = 0;
    for (long i = 0; i < 8; i++)
        created += pthread_create(&t[i], NULL, adder, (void *)(i + 1)) == 0;
    long ok_threads = 0;
    for (int i = 0; i < created; i++) {
        void *r;
        pthread_join(t[i], &r);
        ok_threads += (long)r;
    }
    check("pthread_create x8 + join", created == 8);
    check("mutex: 8 x 20000 increments", counter == 8 * 20000);
    check("thread-local storage and errno per thread", ok_threads == 8 && tls_value == 7);
    pthread_t p;
    pthread_create(&p, NULL, producer, NULL);
    long sum = 0;
    for (int got = 0; got < 200; got++) {
        pthread_mutex_lock(&lock);
        while (qhead == qtail)
            pthread_cond_wait(&cond, &lock);
        sum += queue[qhead];
        qhead = (qhead + 1) % 64;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
    }
    pthread_join(p, NULL);
    check("condition variable producer/consumer", sum == 200 * 201 / 2);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_printf();
    test_malloc();
    test_files();
    test_mmap();
    test_threads();
    printf("posixtest: %d/%d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
