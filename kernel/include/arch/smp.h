#ifndef AURORA_SMP_H
#define AURORA_SMP_H
#include "types.h"

/* Starts the application processors listed in the ACPI MADT (INIT-SIPI-SIPI
 * through a real-mode trampoline below 1 MiB). Returns the number of CPUs
 * online. CPUs that do not respond are logged and skipped. */
uint32_t smp_init(void);
uint32_t smp_failed_cpus(void);

#endif
