/* /dev/display: scan-out from a shared buffer and Bochs DISPI mode setting
 * (drivers/display.h). Runs under the BKL. */
#include "drivers/display.h"
#include "drivers/framebuffer.h"
#include "drivers/pci.h"
#include "core/file.h"
#include "core/process.h"
#include "core/boot.h"
#include "core/log.h"
#include "memory/uvm.h"
#include "memory/virtual.h"
#include "arch/x86_64/io.h"
#include "lib.h"

#define DISPI_INDEX 0x01CE
#define DISPI_DATA 0x01CF
enum {
    DISPI_ID = 0, DISPI_XRES, DISPI_YRES, DISPI_BPP, DISPI_ENABLE, DISPI_BANK,
    DISPI_VIRT_W, DISPI_VIRT_H, DISPI_X_OFF, DISPI_Y_OFF, DISPI_VRAM_64K,
};
#define DISPI_ENABLED 0x01
#define DISPI_GETCAPS 0x02
#define DISPI_LFB 0x40

static struct {
    bool bochs;
    phys_addr_t lfb;
    uint64_t vram;
    uint32_t max_w, max_h;
    uint32_t generation;
    struct file *owner;
    struct shm_obj *buf;
    uint32_t bw, bh, bstride;
    uint64_t presents, pixels;
} D;

static const struct aurora_display_mode candidates[] = {
    { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 }, { 1366, 768 },
    { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
    { 2560, 1440 },
};

static uint16_t dispi_read(uint16_t idx)
{
    outw(DISPI_INDEX, idx);
    return inw(DISPI_DATA);
}

static void dispi_write(uint16_t idx, uint16_t v)
{
    outw(DISPI_INDEX, idx);
    outw(DISPI_DATA, v);
}

void display_init(void)
{
    const struct pci_dev *d = pci_find(0x1234, 0x1111);
    if (!d || !fb_ready())
        return;
    uint16_t id = dispi_read(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0CF)
        return;
    phys_addr_t lfb = d->bar[0] & ~0xFull;
    if (lfb != g_boot.fb.phys) {
        klog("display: bochs LFB %llx is not the boot framebuffer; no mode setting", (unsigned long long)lfb);
        return;
    }
    D.vram = (uint64_t)dispi_read(DISPI_VRAM_64K) * 65536u;
    if (D.vram < 4u << 20)
        D.vram = 4u << 20;
    /* Capabilities: with GETCAPS set, XRES/YRES read back the maxima. */
    uint16_t en = dispi_read(DISPI_ENABLE);
    dispi_write(DISPI_ENABLE, en | DISPI_GETCAPS);
    D.max_w = dispi_read(DISPI_XRES);
    D.max_h = dispi_read(DISPI_YRES);
    dispi_write(DISPI_ENABLE, en);
    if (!D.max_w || !D.max_h) {
        D.max_w = 2560;
        D.max_h = 1600;
    }
    if (!vmm_map_framebuffer(lfb, D.vram)) {
        klog("display: cannot map %llu bytes of video memory", (unsigned long long)D.vram);
        return;
    }
    D.lfb = lfb;
    D.bochs = true;
    klog("display: bochs dispi id %x, %llu KiB vram, max %ux%u", id, (unsigned long long)(D.vram >> 10), D.max_w,
         D.max_h);
}

bool display_can_set_mode(void) { return D.bochs; }
uint32_t display_generation(void) { return D.generation; }
bool display_owned(void) { return D.owner != NULL; }

static bool mode_ok(uint32_t w, uint32_t h)
{
    return w >= 640 && h >= 480 && w <= D.max_w && h <= D.max_h && (w % 8) == 0 &&
           (uint64_t)w * h * 4 <= D.vram;
}

int display_set_mode(uint32_t w, uint32_t h)
{
    if (!D.bochs)
        return AE_NOTSUP;
    if (!mode_ok(w, h))
        return AE_INVAL;
    dispi_write(DISPI_ENABLE, 0);
    dispi_write(DISPI_XRES, (uint16_t)w);
    dispi_write(DISPI_YRES, (uint16_t)h);
    dispi_write(DISPI_BPP, 32);
    dispi_write(DISPI_VIRT_W, (uint16_t)w);
    dispi_write(DISPI_VIRT_H, (uint16_t)h);
    dispi_write(DISPI_X_OFF, 0);
    dispi_write(DISPI_Y_OFF, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB);
    if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h)
        return AE_IO;
    fb_set_geometry(w, h, (uint64_t)w * 4);
    D.generation++;
    klog("display: mode %ux%u", w, h);
    TEST_EVENT("DISPLAY_MODE w=%u h=%u gen=%u", w, h, D.generation);
    return 0;
}

/* ---- the device file --------------------------------------------------------- */

struct file *display_open(int *err)
{
    if (D.owner) {
        *err = AE_BUSY;
        return NULL;
    }
    if (!fb_ready()) {
        *err = AE_NODEV;
        return NULL;
    }
    struct file *f = file_new(FK_DISPLAY, AURORA_O_RDWR);
    if (!f) {
        *err = AE_NOMEM;
        return NULL;
    }
    D.owner = f;
    klog_set_console(false); /* the display server draws the screen now */
    return f;
}

static void detach(void)
{
    if (D.buf)
        shm_put(D.buf);
    D.buf = NULL;
    D.bw = D.bh = D.bstride = 0;
}

void display_close(struct file *f)
{
    if (D.owner != f)
        return;
    detach();
    D.owner = NULL;
    klog_set_console(true);
    klog("display: released by its owner");
}

static void info(struct aurora_display_info *out)
{
    memset(out, 0, sizeof *out);
    out->width = g_fb.width;
    out->height = g_fb.height;
    out->can_set_mode = D.bochs;
    out->generation = D.generation;
    if (D.bochs) {
        for (size_t i = 0; i < ARRAY_LEN(candidates) && out->nmodes < AURORA_DISP_MODES; i++)
            if (mode_ok(candidates[i].width, candidates[i].height))
                out->modes[out->nmodes++] = candidates[i];
    } else {
        out->nmodes = 1;
        out->modes[0].width = g_fb.width;
        out->modes[0].height = g_fb.height;
    }
}

/* Copies one rectangle from the attached buffer, page by page (the shared
 * object is not contiguous in kernel memory). */
static void present_rect(struct aurora_display_rect r)
{
    int32_t mw = (int32_t)MIN(D.bw, g_fb.width), mh = (int32_t)MIN(D.bh, g_fb.height);
    int32_t x0 = MAX(r.x, 0), y0 = MAX(r.y, 0);
    int64_t x1 = MIN((int64_t)r.x + r.w, (int64_t)mw), y1 = MIN((int64_t)r.y + r.h, (int64_t)mh);
    if (x0 >= x1 || y0 >= y1)
        return;
    uint32_t bpp = g_fb.bpp / 8;
    for (int32_t y = y0; y < y1; y++) {
        uint64_t off = ((uint64_t)y * D.bstride + (uint64_t)x0) * 4;
        uint64_t end = ((uint64_t)y * D.bstride + (uint64_t)x1) * 4;
        volatile uint8_t *dst = g_fb.base + (uint64_t)y * g_fb.pitch + (uint64_t)x0 * bpp;
        while (off < end) {
            uint64_t chunk = MIN(end - off, PAGE_SIZE - (off & (PAGE_SIZE - 1)));
            const uint32_t *src = shm_kptr(D.buf, off);
            if (!src)
                return;
            if (g_fb.fast_xrgb) {
                memcpy((void *)dst, src, chunk);
                dst += chunk;
            } else {
                for (uint64_t i = 0; i < chunk / 4; i++) {
                    uint32_t v = fb_pack(src[i]);
                    if (bpp == 4) {
                        *(volatile uint32_t *)dst = v;
                    } else {
                        dst[0] = (uint8_t)v;
                        dst[1] = (uint8_t)(v >> 8);
                        dst[2] = (uint8_t)(v >> 16);
                    }
                    dst += bpp;
                }
            }
            off += chunk;
        }
    }
    D.pixels += (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
}

long display_ioctl(struct process *p, struct file *f, uint64_t req, uint64_t arg)
{
    if (D.owner != f)
        return AE_BADF;
    switch (req) {
    case AURORA_DISP_INFO: {
        struct aurora_display_info i;
        info(&i);
        return copy_to_user(p, arg, &i, sizeof i);
    }
    case AURORA_DISP_SET_MODE: {
        struct aurora_display_mode m;
        int r = copy_from_user(p, &m, arg, sizeof m);
        if (r)
            return r;
        detach(); /* the old buffer no longer matches the screen */
        return display_set_mode(m.width, m.height);
    }
    case AURORA_DISP_ATTACH: {
        struct aurora_display_attach a;
        int r = copy_from_user(p, &a, arg, sizeof a);
        if (r)
            return r;
        if (a.fd < 0) {
            detach();
            return 0;
        }
        struct file *sf = proc_fd_get(p, a.fd);
        if (!sf || sf->kind != FK_SHM)
            return AE_BADF;
        if (!a.width || !a.height || a.stride < a.width || a.width > 16384 || a.height > 16384 ||
            (uint64_t)a.stride * a.height * 4 > sf->shm->npages * PAGE_SIZE)
            return AE_INVAL;
        detach();
        shm_ref(sf->shm);
        D.buf = sf->shm;
        D.bw = a.width;
        D.bh = a.height;
        D.bstride = a.stride;
        return 0;
    }
    case AURORA_DISP_PRESENT: {
        struct aurora_display_present pr;
        int r = copy_from_user(p, &pr, arg, sizeof pr);
        if (r)
            return r;
        if (pr.n > AURORA_DISP_RECTS)
            return AE_INVAL;
        if (!D.buf)
            return AE_INVAL;
        for (uint32_t i = 0; i < pr.n; i++)
            present_rect(pr.r[i]);
        D.presents++;
        return 0;
    }
    default:
        return AE_NOTTY;
    }
}
