#ifndef AURORA_PANIC_H
#define AURORA_PANIC_H
#include "types.h"

struct interrupt_frame;

NORETURN void panic(const char *fmt, ...) PRINTF_LIKE(1, 2);
NORETURN void panic_exception(const struct interrupt_frame *f);

#define KASSERT(cond)                                                                 \
    do {                                                                              \
        if (!(cond))                                                                  \
            panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__);         \
    } while (0)

#endif
