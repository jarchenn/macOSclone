/* Linear framebuffer access. Internally all rendering uses XRGB8888; this
 * module converts to the bootloader-reported channel layout on output. */
#include "drivers/framebuffer.h"
#include "graphics/surface.h"
#include "core/boot.h"
#include "core/log.h"
#include "lib.h"

struct fb_info g_fb;
static bool fb_ok;

bool fb_ready(void) { return fb_ok; }

bool fb_init(void)
{
    if (!g_boot.have_fb) {
        klog("fb: no framebuffer provided by bootloader");
        return false;
    }
    const struct boot_framebuffer *b = &g_boot.fb;
    if (b->bpp != 32 && b->bpp != 24) {
        klog("fb: unsupported %u bpp framebuffer", b->bpp);
        return false;
    }
    if (b->red_size != 8 || b->green_size != 8 || b->blue_size != 8 ||
        b->red_shift + 8u > b->bpp || b->green_shift + 8u > b->bpp || b->blue_shift + 8u > b->bpp) {
        klog("fb: unsupported channel layout r%u@%u g%u@%u b%u@%u", b->red_size, b->red_shift,
             b->green_size, b->green_shift, b->blue_size, b->blue_shift);
        return false;
    }
    if (b->width == 0 || b->height == 0 || b->width > 8192 || b->height > 8192 ||
        b->pitch < b->width * (b->bpp / 8)) {
        klog("fb: implausible geometry %llux%llu pitch %llu", (unsigned long long)b->width,
             (unsigned long long)b->height, (unsigned long long)b->pitch);
        return false;
    }
    g_fb.base = (volatile uint8_t *)phys_to_virt(b->phys);
    g_fb.width = (uint32_t)b->width;
    g_fb.height = (uint32_t)b->height;
    g_fb.pitch = b->pitch;
    g_fb.bpp = b->bpp;
    g_fb.size = b->pitch * b->height;
    g_fb.rs = b->red_shift;
    g_fb.gs = b->green_shift;
    g_fb.bs = b->blue_shift;
    g_fb.rsz = b->red_size;
    g_fb.gsz = b->green_size;
    g_fb.bsz = b->blue_size;
    g_fb.fast_xrgb = b->bpp == 32 && b->red_shift == 16 && b->green_shift == 8 && b->blue_shift == 0;
    fb_ok = true;
    return true;
}

void fb_set_geometry(uint32_t width, uint32_t height, uint64_t pitch)
{
    g_fb.width = width;
    g_fb.height = height;
    g_fb.pitch = pitch;
    g_fb.bpp = 32;
    g_fb.size = pitch * height;
    g_fb.rs = 16;
    g_fb.gs = 8;
    g_fb.bs = 0;
    g_fb.fast_xrgb = true;
}

uint32_t fb_pack(uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (r << g_fb.rs) | (g << g_fb.gs) | (b << g_fb.bs);
}

static inline void store(volatile uint8_t *p, uint32_t v)
{
    if (g_fb.bpp == 32) {
        *(volatile uint32_t *)p = v;
    } else {
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        p[2] = (uint8_t)(v >> 16);
    }
}

void fb_put_pixel(int32_t x, int32_t y, uint32_t xrgb)
{
    uint64_t off;
    if (!fb_ok || !fb_pixel_offset(g_fb.pitch, g_fb.bpp, g_fb.width, g_fb.height, g_fb.size, x, y, &off))
        return;
    store(g_fb.base + off, fb_pack(xrgb));
}

void fb_fill_rect(struct rect r, uint32_t xrgb)
{
    if (!fb_ok)
        return;
    r = rect_intersect(r, rect_make(0, 0, (int32_t)g_fb.width, (int32_t)g_fb.height));
    if (rect_empty(r))
        return;
    uint32_t v = fb_pack(xrgb);
    uint32_t bpp = g_fb.bpp / 8;
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        volatile uint8_t *row = g_fb.base + (uint64_t)y * g_fb.pitch + (uint64_t)r.x * bpp;
        if (bpp == 4) {
            memset32((uint32_t *)row, v, (size_t)r.w);
        } else {
            for (int32_t x = 0; x < r.w; x++)
                store(row + (uint64_t)x * bpp, v);
        }
    }
}

void fb_present(const struct surface *src, struct rect r)
{
    if (!fb_ok)
        return;
    r = rect_intersect(r, rect_make(0, 0, MIN((int32_t)g_fb.width, src->w), MIN((int32_t)g_fb.height, src->h)));
    if (rect_empty(r))
        return;
    uint32_t bpp = g_fb.bpp / 8;
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        const uint32_t *s = src->px + (size_t)y * (size_t)src->stride + (size_t)r.x;
        volatile uint8_t *d = g_fb.base + (uint64_t)y * g_fb.pitch + (uint64_t)r.x * bpp;
        if (g_fb.fast_xrgb) {
            memcpy((void *)d, s, (size_t)r.w * 4);
        } else {
            for (int32_t x = 0; x < r.w; x++)
                store(d + (uint64_t)x * bpp, fb_pack(s[x]));
        }
    }
}
