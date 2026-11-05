#ifndef AURORA_FRAMEBUFFER_H
#define AURORA_FRAMEBUFFER_H
#include "types.h"
#include "graphics/rect.h"

struct surface;

struct fb_info {
    volatile uint8_t *base; /* virtual address (HHDM) */
    uint32_t width, height;
    uint64_t pitch;
    uint32_t bpp;
    uint64_t size; /* pitch * height */
    uint8_t rs, gs, bs; /* channel shifts */
    uint8_t rsz, gsz, bsz;
    bool fast_xrgb; /* 32bpp with R=16,G=8,B=0, 8-bit channels */
};

extern struct fb_info g_fb;

/* Validates the bootloader framebuffer. Returns false (with a serial
 * diagnostic) for formats the renderer does not support. */
bool fb_init(void);
bool fb_ready(void);
uint32_t fb_pack(uint32_t xrgb);
void fb_put_pixel(int32_t x, int32_t y, uint32_t xrgb);
void fb_fill_rect(struct rect r, uint32_t xrgb);
/* Copies rect r of a screen-sized XRGB8888 surface to the framebuffer,
 * converting the pixel format. */
void fb_present(const struct surface *src, struct rect r);

#endif
