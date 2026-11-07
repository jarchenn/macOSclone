#include "gui/desktop.h"
#include "lib.h"

void widget_button(struct surface *s, struct rect r, const char *label, int style, bool hover)
{
    const struct theme *t = g_theme;
    uint32_t bg = t->button_bg, fg = t->button_text, border = t->button_border;
    if (style == BTN_PRIMARY) {
        bg = t->accent;
        fg = t->accent_text;
        border = t->accent;
    } else if (style == BTN_DANGER) {
        bg = t->danger;
        fg = RGB(0xFF, 0xFF, 0xFF);
        border = t->danger;
    } else if (hover) {
        bg = t->button_hover;
    }
    if (hover && style != BTN_NORMAL)
        bg = color_mix(bg, RGB(0xFF, 0xFF, 0xFF), 30);
    draw_round_rect(s, rect_make(r.x, r.y + 1, r.w, r.h), 6, RGB(0, 0, 0), 25, 0xF);
    draw_round_rect(s, r, 6, bg, 255, 0xF);
    draw_round_frame(s, r, 6, border, 255);
    int32_t tw = text_width(FONT_UI, label);
    text_draw(s, FONT_UI, r.x + (r.w - tw) / 2, r.y + (r.h - text_height(FONT_UI)) / 2, label, fg);
}

void widget_field(struct surface *s, struct rect r, const char *text, bool focused, bool caret)
{
    const struct theme *t = g_theme;
    draw_round_rect(s, r, 5, t->field_bg, 255, 0xF);
    draw_round_frame(s, r, 5, focused ? t->accent : t->field_border, 255);
    if (focused)
        draw_round_frame(s, rect_inset(r, -2), 7, t->accent, 70);
    /* Show the end of long text (where the caret is). */
    size_t len = strlen(text);
    const char *shown = text;
    while (*shown && text_width(FONT_UI, shown) > r.w - 18)
        shown += utf8_next(shown, len - (size_t)(shown - text), 0);
    struct rect old = s->clip;
    surface_set_clip(s, rect_intersect(old, rect_inset(r, 2)));
    int32_t w = text_draw(s, FONT_UI, r.x + 8, r.y + (r.h - text_height(FONT_UI)) / 2, shown, t->text);
    s->clip = old;
    if (caret)
        draw_fill(s, rect_make(r.x + 8 + w, r.y + 5, 2, r.h - 10), t->accent);
}

void widget_separator(struct surface *s, int32_t x, int32_t y, int32_t w) { draw_hline(s, x, y, w, g_theme->separator); }

void widget_badge(struct surface *s, int32_t x, int32_t y, const char *text, uint32_t bg, uint32_t fg)
{
    int32_t w = text_width(FONT_SMALL, text) + 12;
    draw_round_rect(s, rect_make(x, y, w, 18), 9, bg, 255, 0xF);
    text_draw(s, FONT_SMALL, x + 6, y + (18 - text_height(FONT_SMALL)) / 2, text, fg);
}
