#ifndef AURORA_APP_UTIL_H
#define AURORA_APP_UTIL_H
#include "types.h"
#include "graphics/surface.h"

const char *app_firmware_name(void);
/* One-line, accurate description of where /home files are kept. */
const char *app_storage_note(void);
/* Word-wraps `text` into the given width, calling draw for each line;
 * returns the y coordinate after the last line. */
int32_t app_draw_wrapped(struct surface *s, const struct font_data *f, int32_t x, int32_t y, int32_t w,
                         int32_t line_h, const char *text, uint32_t color);
void app_draw_folder_icon(struct surface *s, int32_t x, int32_t y);
void app_draw_file_icon(struct surface *s, int32_t x, int32_t y);

#endif
