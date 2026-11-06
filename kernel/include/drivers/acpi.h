#ifndef AURORA_ACPI_H
#define AURORA_ACPI_H
#include "types.h"

struct acpi_info;

/* ACPI: table discovery and parsing (MADT, FADT, HPET), S5 power-off, the
 * FADT reset register and the fixed power button. No AML interpreter: \_S5
 * is found by a byte scan of the DSDT. */
bool acpi_init(void);
const struct acpi_info *acpi_info(void); /* NULL if no valid tables */
bool acpi_available(void);               /* S5 power-off possible */
/* Enters S5. Returns only if the hardware ignored the request. */
void acpi_poweroff(void);
/* Writes the FADT reset register; returns false if unsupported or ignored. */
bool acpi_reset(void);
/* Fixed-feature power button: enable the event, and poll/clear its status
 * (called from the SCI handler). */
bool acpi_enable_power_button(void);
bool acpi_power_button_pressed(void);
uint16_t acpi_sci_irq(void);
bool acpi_setup_sci(void); /* routes the SCI and enables the power button */
const char *acpi_describe(void);

#endif
