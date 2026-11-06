#include "core/log.h"
#include "drivers/serial.h"
#include "graphics/console.h"
#include "arch/x86_64/io.h"
#include "lib.h"

static bool console_enabled;

void klog_set_console(bool enabled) { console_enabled = enabled; }

/* Raw (not lockdep-tracked) lock so lines from different CPUs do not
 * interleave; lockdep itself logs through here. */
static volatile uint32_t log_lock;

static uint64_t log_acquire(void)
{
    uint64_t f = irq_save();
    while (__atomic_exchange_n(&log_lock, 1, __ATOMIC_ACQUIRE))
        cpu_pause();
    return f;
}

static void log_release(uint64_t f)
{
    __atomic_store_n(&log_lock, 0, __ATOMIC_RELEASE);
    irq_restore(f);
}

void klog_raw(const char *s)
{
    uint64_t f = log_acquire();
    serial_write(s, strlen(s));
    if (console_enabled)
        console_write(s);
    log_release(f);
}

/* Panic path: never waits for a CPU that may have died holding the lock. */
void klog_force_unlock(void) { __atomic_store_n(&log_lock, 0, __ATOMIC_RELEASE); }

void klog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = k_vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n] = '\n';
    buf[n + 1] = '\0';
    klog_raw(buf);
}

#ifdef AURORA_TEST
void test_event(const char *fmt, ...)
{
    char buf[320];
    buf[0] = '@';
    buf[1] = '@';
    va_list ap;
    va_start(ap, fmt);
    int n = k_vsnprintf(buf + 2, sizeof buf - 4, fmt, ap);
    va_end(ap);
    n += 2;
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;
    buf[n] = '\n';
    buf[n + 1] = '\0';
    uint64_t f = log_acquire();
    serial_write(buf, (size_t)n + 1);
    log_release(f);
}
#endif
