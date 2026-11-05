#ifndef AURORA_CONSOLE_H
#define AURORA_CONSOLE_H
#include "types.h"

/* Framebuffer diagnostic console used during early boot and for panics.
 * Draws directly to the framebuffer, never allocates. */
void console_init(void);
void console_write(const char *s);
/* Clears the screen to the panic colour scheme and resets the cursor. */
void console_begin_panic(void);

#endif
