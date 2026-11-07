#ifndef AURORA_LOG_H
#define AURORA_LOG_H
#include "types.h"

/* Kernel log: COM1 serial plus (until the desktop starts) the framebuffer
 * diagnostic console. Not for use from IRQ context. */
void klog(const char *fmt, ...) PRINTF_LIKE(1, 2);
void klog_raw(const char *s);
void klog_set_console(bool enabled);
void klog_force_unlock(void);

#ifdef AURORA_TEST
/* Test-only structured markers ("@@NAME k=v ...") consumed by the tools/ test scripts */
void test_event(const char *fmt, ...) PRINTF_LIKE(1, 2);
#define TEST_EVENT(...) test_event(__VA_ARGS__)
#else
#define TEST_EVENT(...) ((void)0)
#endif

#endif
