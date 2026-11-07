#ifndef AURORA_APP_UTIL_H
#define AURORA_APP_UTIL_H
#include "types.h"
#include "graphics/surface.h"
#include "gui/text.h"

struct window;

const char *app_firmware_name(void);
/* One-line, accurate description of where /home files are kept. */
const char *app_storage_note(void);
/* Word-wraps `text` into the given width; returns the y after the last line. */
int32_t app_draw_wrapped(struct surface *s, const struct text_font *f, int32_t x, int32_t y, int32_t w,
                         int32_t line_h, const char *text, uint32_t color);
void app_draw_folder_icon(struct surface *s, int32_t x, int32_t y);
void app_draw_file_icon(struct surface *s, int32_t x, int32_t y);
/* main() of a single-window app: connect, open(argv[1]), run. */
int app_main(int app, const char *name, struct window *(*open)(const char *arg), int argc, char **argv);

#endif
