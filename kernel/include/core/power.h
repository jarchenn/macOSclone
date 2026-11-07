#ifndef AURORA_POWER_H
#define AURORA_POWER_H
#include "types.h"

#define POWER_RESTART 1
#define POWER_OFF 2

/* Safe from IRQ context (the ACPI SCI) and system calls. */
void power_request(int how);
/* Task 0 after boot: serves power requests; never returns. */
NORETURN void power_main(void);

#endif
