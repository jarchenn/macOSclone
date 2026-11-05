#ifndef AURORA_SURFACE_H
#define AURORA_SURFACE_H
#include "types.h"
#include "graphics/rect.h"
#include "graphics/font.h"

/* A 32-bit XRGB8888 pixel buffer. All drawing is clipped to `clip` (which is
 * itself always kept inside the surface bounds). */
struct surface {
    uint32_t *px;
    int32_t w, h;
    int32_t stride; /* in pixels */
    struct rect clip;
};

#define RGB(r, g, b) ((uint32_t)(((r) & 0xFF) << 16 | ((g) & 0xFF) << 8 | ((b) & 0xFF)))

/* Allocates pixel storage from the kernel heap. Returns false on overflow or
 * allocation failure; the surface is left zeroed in that case. */
bool surface_alloc(struct surface *s, int32_t w, int32_t h);
void surface_free(struct surface *s);
void surface_set_clip(struct surface *s, struct rect clip);
void surface_reset_clip(struct surface *s);

uint32_t color_blend(uint32_t dst, uint32_t src, uint32_t alpha); /* alpha 0..255 */
uint32_t color_mix(uint32_t a, uint32_t b, uint32_t t);            /* t 0..255 */

void draw_fill(struct surface *s, struct rect r, uint32_t c);
void draw_blend(struct surface *s, struct rect r, uint32_t c, uint32_t alpha);
void draw_hline(struct surface *s, int32_t x, int32_t y, int32_t w, uint32_t c);
void draw_vline(struct surface *s, int32_t x, int32_t y, int32_t h, uint32_t c);
void draw_frame(struct surface *s, struct rect r, uint32_t c);
void draw_pixel_blend(struct surface *s, int32_t x, int32_t y, uint32_t c, uint32_t alpha);
void draw_vgradient(struct surface *s, struct rect r, uint32_t top, uint32_t bottom);
/* Anti-aliased rounded rectangle (radius <= 32). corners: bit0 TL, bit1 TR,
 * bit2 BL, bit3 BR; alpha is the overall opacity 0..255. */
void draw_round_rect(struct surface *s, struct rect r, int32_t radius, uint32_t c, uint32_t alpha, unsigned corners);
void draw_round_rect_gradient(struct surface *s, struct rect r, int32_t radius, uint32_t top, uint32_t bottom);
void draw_round_frame(struct surface *s, struct rect r, int32_t radius, uint32_t c, uint32_t alpha);
void draw_circle(struct surface *s, int32_t cx, int32_t cy, int32_t radius, uint32_t c, uint32_t alpha);
/* Soft drop shadow around r (does not paint inside r). */
void draw_shadow(struct surface *s, struct rect r, int32_t radius, int32_t spread, uint32_t max_alpha);

void blit(struct surface *dst, const struct surface *src, int32_t dx, int32_t dy);
/* Blit with the bottom two corners rounded (anti-aliased against dst). */
void blit_round_bottom(struct surface *dst, const struct surface *src, int32_t dx, int32_t dy, int32_t radius);

int32_t draw_text(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, uint32_t c);
int32_t draw_text_n(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, size_t n, uint32_t c);
int32_t draw_text_bold(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, uint32_t c);
int32_t text_width(const struct font_data *f, const char *text);
/* Draws text truncated with "..." to fit max_w pixels. */
void draw_text_fit(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, int32_t max_w, uint32_t c);

#define FONT (&font_spleen_8x16)
#define FONT_BIG (&font_spleen_12x24)

#endif
