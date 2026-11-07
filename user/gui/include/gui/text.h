#ifndef AURORA_GUI_TEXT_H
#define AURORA_GUI_TEXT_H
/* Text rendering (0.7b): TrueType outlines rasterized by stb_truetype into
 * a glyph cache per font and size, grayscale anti-aliasing blended in
 * linear light (gamma-correct), pair kerning, UTF-8 input. If a font file
 * cannot be loaded the 0.1 bitmap fonts (Spleen) stand in. Latin scripts
 * only: no shaping, no right-to-left. */
#include "types.h"
#include "graphics/surface.h"

struct text_font;

/* The fonts every program gets after text_init(). Sizes in pixels. */
extern struct text_font *const FONT_UI;      /* DejaVu Sans 13 px: body text */
extern struct text_font *const FONT_UI_BOLD; /* DejaVu Sans Bold 13 px */
extern struct text_font *const FONT_SMALL;   /* DejaVu Sans 11 px */
extern struct text_font *const FONT_TITLE;   /* DejaVu Sans Bold 20 px */
extern struct text_font *const FONT_MONO;    /* DejaVu Sans Mono 13 px: terminal, editor */

/* Loads the font files from /system/fonts (once). Returns false if any
 * fell back to the bitmap font. */
bool text_init(void);
/* A font at another size (cached; at most 8 extra sizes per face). face:
 * 0 sans, 1 sans bold, 2 mono. */
struct text_font *text_font_at(int face, int px);
bool text_font_is_bitmap(const struct text_font *f);

/* Metrics: the line box is `height` pixels tall; text is drawn with its
 * top at y (baseline at y + ascent). */
int32_t text_height(const struct text_font *f);
int32_t text_ascent(const struct text_font *f);
/* Advance of one cell for monospaced fonts (and the average for others). */
int32_t text_cell_width(const struct text_font *f);

/* Draws UTF-8 text; returns the advance in pixels. Invalid UTF-8 bytes
 * draw as U+FFFD; missing glyphs as the font's .notdef. */
int32_t text_draw(struct surface *s, const struct text_font *f, int32_t x, int32_t y, const char *utf8, uint32_t c);
int32_t text_draw_n(struct surface *s, const struct text_font *f, int32_t x, int32_t y, const char *utf8, size_t n,
                    uint32_t c);
/* One code point (terminal cells). */
int32_t text_draw_cp(struct surface *s, const struct text_font *f, int32_t x, int32_t y, uint32_t cp, uint32_t c);
int32_t text_width(const struct text_font *f, const char *utf8);
int32_t text_width_n(const struct text_font *f, const char *utf8, size_t n);
/* Draws text truncated with an ellipsis to fit max_w. */
void text_draw_fit(struct surface *s, const struct text_font *f, int32_t x, int32_t y, const char *utf8, int32_t max_w,
                   uint32_t c);

/* Layout: bytes of `utf8` (at most n) that fit in max_w pixels, breaking
 * after the last space when `words` is set. Never splits a code point. */
size_t text_fit(const struct text_font *f, const char *utf8, size_t n, int32_t max_w, bool words);
/* X offset of byte index `i` within the first n bytes (caret placement). */
int32_t text_offset(const struct text_font *f, const char *utf8, size_t n, size_t i);
/* Byte index nearest to x within the first n bytes (mouse hit-testing). */
size_t text_index_at(const struct text_font *f, const char *utf8, size_t n, int32_t x);
/* Word-wrapped paragraph(s); s may be NULL to measure. Returns the y after
 * the last line. */
int32_t text_wrap(struct surface *s, const struct text_font *f, int32_t x, int32_t y, int32_t w, int32_t line_h,
                  const char *utf8, uint32_t c);

/* UTF-8 helpers. */
uint32_t utf8_decode(const char *s, size_t n, size_t *len); /* U+FFFD on error, *len >= 1 */
size_t utf8_encode(uint32_t cp, char out[4]);
size_t utf8_prev(const char *s, size_t i); /* start of the code point before i */
size_t utf8_next(const char *s, size_t n, size_t i);

/* Statistics for tests: glyph cache hits/misses and rasterized glyphs. */
void text_stats(uint64_t *hits, uint64_t *misses, uint64_t *glyphs);

#endif
