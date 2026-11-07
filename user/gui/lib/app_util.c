#include "apps/app_util.h"
#include "desktop/desktop.h"
#include "fs/vfs.h"
#include "lib.h"
#include <aurora.h>
#include <string.h>

const char *app_firmware_name(void)
{
    struct aurora_klog k;
    memset(&k, 0, sizeof k);
    aurora_klog(&k);
    switch (k.firmware) {
    case 2: return "UEFI (x86-64)";
    case 1: return "BIOS";
    case 3: return "UEFI (32-bit)";
    default: return "unknown";
    }
}

const char *app_storage_note(void)
{
    return vfs_home_persistent() ? "Files in /home are saved to the data disk and survive restarts."
                                 : "Files are stored in memory and reset when Aurora restarts.";
}

int32_t app_draw_wrapped(struct surface *s, const struct text_font *f, int32_t x, int32_t y, int32_t w,
                         int32_t line_h, const char *text, uint32_t color)
{
    return text_wrap(s, f, x, y, w, line_h, text, color);
}

void app_draw_folder_icon(struct surface *s, int32_t x, int32_t y)
{
    draw_round_rect(s, rect_make(x, y + 1, 8, 5), 2, RGB(0x6F, 0xA8, 0xF5), 255, 0xF);
    draw_round_rect(s, rect_make(x, y + 3, 18, 12), 2, RGB(0x4E, 0x8F, 0xEE), 255, 0xF);
    draw_round_rect(s, rect_make(x, y + 6, 18, 9), 2, RGB(0x7D, 0xB4, 0xFF), 255, 0xF);
}

void app_draw_file_icon(struct surface *s, int32_t x, int32_t y)
{
    draw_round_rect(s, rect_make(x + 2, y, 14, 16), 2, RGB(0xFF, 0xFF, 0xFF), 255, 0xF);
    draw_round_frame(s, rect_make(x + 2, y, 14, 16), 2, RGB(0x9A, 0x9A, 0xA8), 255);
    for (int i = 0; i < 3; i++)
        draw_fill(s, rect_make(x + 5, y + 5 + i * 3, 8, 1), RGB(0xB0, 0xB0, 0xBC));
}

/* Single-instance apps: a second launch (from the dock, a shortcut or
 * another app) reaches the running instance through the server. */
int app_main(int app, const char *name, struct window *(*open)(const char *arg), int argc, char **argv)
{
    if (!ui_init(app, name))
        return 1;
    ui_on_open(open);
    if (!open(argc > 1 ? argv[1] : NULL))
        return 1;
    return ui_run();
}
