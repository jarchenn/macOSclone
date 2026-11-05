#ifndef AURORA_SERIAL_H
#define AURORA_SERIAL_H
#include "types.h"
bool serial_init(void);
bool serial_present(void);
void serial_putc(char c);
void serial_write(const char *s, size_t n);
#endif
