#include "core/log.h"
#include "drivers/serial.h"
#include "graphics/console.h"
#include "arch/x86_64/io.h"
#include "lib.h"

static bool console_enabled;

void klog_set_console(bool enabled) { console_enabled = enabled; }

void klog_raw(const char *s)
{
    uint64_t f = irq_save();
    serial_write(s, strlen(s));
    if (console_enabled)
        console_write(s);
    irq_restore(f);
}

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
    uint64_t f = irq_save();
    serial_write(buf, (size_t)n + 1);
    irq_restore(f);
}
#endif
