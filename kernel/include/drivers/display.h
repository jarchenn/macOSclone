#ifndef AURORA_DISPLAY_H
#define AURORA_DISPLAY_H
#include "types.h"

/* /dev/display (0.7). One process (the display server) owns it at a time.
 * It composes into a shared-memory XRGB8888 buffer and asks the kernel to
 * present damaged rectangles; the kernel converts to the scan-out format.
 * Resolution changes go through the Bochs display adapter (DISPI
 * registers, QEMU -vga std) when the boot framebuffer is its LFB. */

struct file;
struct process;

void display_init(void);
bool display_can_set_mode(void);
/* Changes the mode (32 bpp); the kernel console and g_fb follow. */
int display_set_mode(uint32_t w, uint32_t h);
uint32_t display_generation(void);

struct file *display_open(int *err);
void display_close(struct file *f);
long display_ioctl(struct process *p, struct file *f, uint64_t req, uint64_t arg);
/* True while a user-space display server owns the screen. */
bool display_owned(void);

#endif
