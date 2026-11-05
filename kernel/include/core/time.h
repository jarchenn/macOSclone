#ifndef AURORA_TIME_H
#define AURORA_TIME_H
#include "types.h"

/* PIT channel 0 drives a monotonic tick counter. Elapsed time is derived
 * from the programmed divisor and the PIT's 1.193182 MHz input clock, not
 * from an assumed nominal rate. */
#define PIT_INPUT_HZ 1193182u
#define PIT_TARGET_HZ 250u

void pit_init(void);
uint64_t time_ticks(void);
uint64_t time_ms(void);
uint32_t pit_divisor(void);
/* Busy-wait (interrupts must be enabled). */
void time_sleep_ms(uint32_t ms);

#endif
