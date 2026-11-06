#ifndef AURORA_ACPI_H
#define AURORA_ACPI_H
#include "types.h"

/* Minimal ACPI: finds the FADT and the \_S5 sleep type in the DSDT so the
 * machine can be powered off (S5). No AML interpreter. */
bool acpi_init(void);
bool acpi_available(void);
/* Enters S5. Returns only if the hardware ignored the request. */
void acpi_poweroff(void);
const char *acpi_describe(void);

#endif
