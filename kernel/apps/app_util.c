#include "apps/app_util.h"
#include "desktop/desktop.h"
#include "core/boot.h"
#include "limine.h"
#include "fs/vfs.h"
#include "lib.h"

const char *app_firmware_name(void)
{
    switch (g_boot.firmware_type) {
    case LIMINE_FIRMWARE_TYPE_EFI64: return "UEFI (x86-64)";
    case LIMINE_FIRMWARE_TYPE_X86BIOS: return "BIOS";
    case LIMINE_FIRMWARE_TYPE_EFI32: return "UEFI (32-bit)";
    default: return "unknown";
    }
}

const char *app_storage_note(void)
{
    return vfs_home_persistent() ? "Files in /home are saved to the data disk and survive restarts."
                                 : "Files are stored in memory and reset when Aurora restarts.";
}

int32_t app_draw_wrapped(struct surface *s, const struct font_data *f, int32_t x, int32_t y, int32_t w,
                         int32_t line_h, const char *text, uint32_t color)
{
    int32_t maxc = w / f->width;
    if (maxc < 1)
        return y;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t off = 0;
        if (len == 0)
            y += line_h;
        while (off < len) {
            size_t n = len - off;
            if (n > (size_t)maxc) {
                /* break at the last space that fits */
                size_t cut = (size_t)maxc;
                while (cut > 0 && p[off + cut] != ' ')
                    cut--;
                n = cut ? cut : (size_t)maxc;
            }
            draw_text_n(s, f, x, y, p + off, n, color);
            y += line_h;
            off += n;
            while (off < len && p[off] == ' ')
                off++;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return y;
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
