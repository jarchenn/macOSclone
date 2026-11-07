#ifndef AURORA_GUI_DESKTOP_H
#define AURORA_GUI_DESKTOP_H
/* Shared by the display server and every app (0.7): layout constants, the
 * theme, text rendering and the small drawing widgets. Pixels are XRGB8888
 * everywhere (graphics/surface.h, shared with the kernel's console). */
#include "types.h"
#include "graphics/surface.h"
#include "gui/text.h"

/* ---- Layout constants ---------------------------------------------------- */
#define MENUBAR_H 26
#define TITLE_H 30
#define WIN_RADIUS 10
#define DOCK_ICON 52
#define DOCK_GAP 12
#define DOCK_PAD 10
#define DOCK_MARGIN 8
#define MAX_WINDOWS 32

/* Title-bar controls: centres relative to the window frame origin. */
#define CTRL_Y 15
#define CTRL_CLOSE_X 20
#define CTRL_MIN_X 40
#define CTRL_MAX_X 60
#define CTRL_R 6

#define WM_GRIP 16
#define WM_MIN_W 160 /* frame size floor; apps may ask for more */
#define WM_MIN_H 100

enum app_id { APP_TERMINAL = 0, APP_FILES, APP_NOTES, APP_SETTINGS, APP_WELCOME, APP_OTHER, APP_COUNT };
extern const char *const app_names[APP_COUNT];
/* Program in /system/bin for each app (dock order matches APP_*). */
extern const char *const app_programs[APP_COUNT];
int app_from_name(const char *name);

/* ---- Theme ------------------------------------------------------------- */
struct theme {
    bool dark;
    uint32_t win_bg, win_border, title_active, title_inactive, title_text, title_text_inactive;
    uint32_t text, text_dim, accent, accent_text, selection, sel_text;
    uint32_t field_bg, field_border, button_bg, button_border, button_text, button_hover;
    uint32_t menubar_bg, menubar_text;
    uint32_t dock_bg, dock_border;
    uint32_t term_bg, term_text, term_prompt, term_error;
    uint32_t separator, sidebar_bg, warning_bg, warning_text, danger;
    uint32_t ctrl_inactive;
    uint8_t menubar_alpha, dock_alpha, border_alpha;
};
extern const struct theme *g_theme;
void theme_set(bool dark);
void wallpaper_generate(struct surface *s);
void icon_draw(struct surface *s, int app, int32_t x, int32_t y, bool pressed);
void logo_draw(struct surface *s, int32_t cx, int32_t cy);

/* ---- Widgets (drawing only) ---------------------------------------------- */
enum button_style { BTN_NORMAL = 0, BTN_PRIMARY, BTN_DANGER };
void widget_button(struct surface *s, struct rect r, const char *label, int style, bool hover);
void widget_field(struct surface *s, struct rect r, const char *text, bool focused, bool caret);
void widget_separator(struct surface *s, int32_t x, int32_t y, int32_t w);
void widget_badge(struct surface *s, int32_t x, int32_t y, const char *text, uint32_t bg, uint32_t fg);

/* Test markers ("@@..." on the serial log of test builds). */
void aurora_test_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef AURORA_TEST
#define TEST_EVENT(...) aurora_test_event(__VA_ARGS__)
#else
/* Compiled out, but the arguments still count as used (no warnings). */
#define TEST_EVENT(...) do { if (0) aurora_test_event(__VA_ARGS__); } while (0)
#endif

uint64_t gui_now_ms(void);

#endif
