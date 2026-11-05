/* Software rendering primitives on XRGB8888 surfaces. Integer math only. */
#include "graphics/surface.h"
#include "kalloc.h"
#include "lib.h"

bool surface_alloc(struct surface *s, int32_t w, int32_t h)
{
    size_t bytes;
    memset(s, 0, sizeof *s);
    if (!surface_size_bytes(w, h, &bytes))
        return false;
    s->px = kmalloc(bytes);
    if (!s->px)
        return false;
    s->w = w;
    s->h = h;
    s->stride = w;
    surface_reset_clip(s);
    return true;
}

void surface_free(struct surface *s)
{
    if (s->px)
        kfree(s->px);
    memset(s, 0, sizeof *s);
}

void surface_set_clip(struct surface *s, struct rect clip)
{
    s->clip = rect_intersect(clip, rect_make(0, 0, s->w, s->h));
}

void surface_reset_clip(struct surface *s) { s->clip = rect_make(0, 0, s->w, s->h); }

uint32_t color_blend(uint32_t dst, uint32_t src, uint32_t a)
{
    if (a >= 255)
        return src;
    if (a == 0)
        return dst;
    uint32_t ia = 255 - a;
    uint32_t rb = ((src & 0xFF00FF) * a + (dst & 0xFF00FF) * ia + 0x800080) >> 8;
    uint32_t g = ((src & 0x00FF00) * a + (dst & 0x00FF00) * ia + 0x008000) >> 8;
    return (rb & 0xFF00FF) | (g & 0x00FF00);
}

uint32_t color_mix(uint32_t a, uint32_t b, uint32_t t) { return color_blend(a, b, t); }

static inline uint32_t *px_at(struct surface *s, int32_t x, int32_t y)
{
    return s->px + (size_t)y * (size_t)s->stride + (size_t)x;
}

void draw_fill(struct surface *s, struct rect r, uint32_t c)
{
    r = rect_intersect(r, s->clip);
    if (rect_empty(r))
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        memset32(px_at(s, r.x, y), c, (size_t)r.w);
}

void draw_blend(struct surface *s, struct rect r, uint32_t c, uint32_t alpha)
{
    if (alpha >= 255) {
        draw_fill(s, r, c);
        return;
    }
    r = rect_intersect(r, s->clip);
    if (rect_empty(r) || alpha == 0)
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        uint32_t *p = px_at(s, r.x, y);
        for (int32_t x = 0; x < r.w; x++)
            p[x] = color_blend(p[x], c, alpha);
    }
}

void draw_hline(struct surface *s, int32_t x, int32_t y, int32_t w, uint32_t c) { draw_fill(s, rect_make(x, y, w, 1), c); }
void draw_vline(struct surface *s, int32_t x, int32_t y, int32_t h, uint32_t c) { draw_fill(s, rect_make(x, y, 1, h), c); }

void draw_frame(struct surface *s, struct rect r, uint32_t c)
{
    draw_hline(s, r.x, r.y, r.w, c);
    draw_hline(s, r.x, r.y + r.h - 1, r.w, c);
    draw_vline(s, r.x, r.y, r.h, c);
    draw_vline(s, r.x + r.w - 1, r.y, r.h, c);
}

void draw_pixel_blend(struct surface *s, int32_t x, int32_t y, uint32_t c, uint32_t alpha)
{
    if (!rect_contains(s->clip, x, y) || alpha == 0)
        return;
    uint32_t *p = px_at(s, x, y);
    *p = color_blend(*p, c, alpha);
}

void draw_vgradient(struct surface *s, struct rect r, uint32_t top, uint32_t bottom)
{
    if (r.h <= 0)
        return;
    struct rect cr = rect_intersect(r, s->clip);
    for (int32_t y = cr.y; y < cr.y + cr.h; y++) {
        uint32_t t = r.h > 1 ? (uint32_t)((int64_t)(y - r.y) * 255 / (r.h - 1)) : 0;
        memset32(px_at(s, cr.x, y), color_mix(top, bottom, t), (size_t)cr.w);
    }
}

/* ---- Anti-aliased corners ---------------------------------------------- */

/* Coverage (0..255) of pixel (i, j) in the top-left r x r corner square of a
 * circle with radius `rad` centred at (cx, cx) (all in pixels), using 4x4
 * supersampling in 1/8-pixel units. */
static uint32_t corner_cov(int32_t i, int32_t j, int32_t cx, int32_t rad)
{
    if (rad <= 0)
        return 0;
    int32_t c8 = cx * 8, r8 = rad * 8;
    int64_t rr = (int64_t)r8 * r8;
    int n = 0;
    for (int sy = 0; sy < 4; sy++) {
        int64_t dy = (int64_t)j * 8 + sy * 2 + 1 - c8;
        for (int sx = 0; sx < 4; sx++) {
            int64_t dx = (int64_t)i * 8 + sx * 2 + 1 - c8;
            if (dx * dx + dy * dy <= rr)
                n++;
        }
    }
    return (uint32_t)(n * 255 / 16);
}

#define COV_CACHE 4
static struct {
    int32_t radius;
    uint8_t cov[32][32];
} cov_cache[COV_CACHE];
static int cov_next;

static const uint8_t (*cov_table(int32_t radius))[32]
{
    for (int i = 0; i < COV_CACHE; i++)
        if (cov_cache[i].radius == radius)
            return (const uint8_t (*)[32])cov_cache[i].cov;
    int slot = cov_next++ % COV_CACHE;
    cov_cache[slot].radius = radius;
    for (int32_t j = 0; j < radius; j++)
        for (int32_t i = 0; i < radius; i++)
            cov_cache[slot].cov[j][i] = (uint8_t)corner_cov(i, j, radius, radius);
    return (const uint8_t (*)[32])cov_cache[slot].cov;
}

/* Coverage of pixel (x, y) inside rect r with corner radius rad. */
static inline uint32_t rr_cov(const uint8_t (*t)[32], struct rect r, int32_t rad, unsigned corners, int32_t x, int32_t y)
{
    int32_t lx = x - r.x, ly = y - r.y;
    int32_t rx = r.x + r.w - 1 - x, ry = r.y + r.h - 1 - y;
    if (ly < rad) {
        if (lx < rad && (corners & 1))
            return t[ly][lx];
        if (rx < rad && (corners & 2))
            return t[ly][rx];
    }
    if (ry < rad) {
        if (lx < rad && (corners & 4))
            return t[ry][lx];
        if (rx < rad && (corners & 8))
            return t[ry][rx];
    }
    return 255;
}

void draw_round_rect(struct surface *s, struct rect r, int32_t radius, uint32_t c, uint32_t alpha, unsigned corners)
{
    if (radius > 32)
        radius = 32;
    if (radius > r.w / 2)
        radius = r.w / 2;
    if (radius > r.h / 2)
        radius = r.h / 2;
    if (radius <= 0) {
        draw_blend(s, r, c, alpha);
        return;
    }
    const uint8_t (*t)[32] = cov_table(radius);
    struct rect cr = rect_intersect(r, s->clip);
    for (int32_t y = cr.y; y < cr.y + cr.h; y++) {
        bool band = (y - r.y) < radius || (r.y + r.h - 1 - y) < radius;
        if (!band) {
            draw_blend(s, rect_make(cr.x, y, cr.w, 1), c, alpha);
            continue;
        }
        uint32_t *p = px_at(s, 0, y);
        for (int32_t x = cr.x; x < cr.x + cr.w; x++) {
            uint32_t cov = rr_cov(t, r, radius, corners, x, y);
            uint32_t a = cov * alpha / 255;
            if (a)
                p[x] = color_blend(p[x], c, a);
        }
    }
}

void draw_round_rect_gradient(struct surface *s, struct rect r, int32_t radius, uint32_t top, uint32_t bottom)
{
    if (radius > 32)
        radius = 32;
    if (radius > r.w / 2)
        radius = r.w / 2;
    if (radius > r.h / 2)
        radius = r.h / 2;
    const uint8_t (*t)[32] = radius > 0 ? cov_table(radius) : NULL;
    struct rect cr = rect_intersect(r, s->clip);
    for (int32_t y = cr.y; y < cr.y + cr.h; y++) {
        uint32_t tt = r.h > 1 ? (uint32_t)((int64_t)(y - r.y) * 255 / (r.h - 1)) : 0;
        uint32_t c = color_mix(top, bottom, tt);
        uint32_t *p = px_at(s, 0, y);
        for (int32_t x = cr.x; x < cr.x + cr.w; x++) {
            uint32_t cov = t ? rr_cov(t, r, radius, 0xF, x, y) : 255;
            if (cov)
                p[x] = color_blend(p[x], c, cov);
        }
    }
}

void draw_round_frame(struct surface *s, struct rect r, int32_t radius, uint32_t c, uint32_t alpha)
{
    if (radius > 32)
        radius = 32;
    if (radius > r.w / 2)
        radius = r.w / 2;
    if (radius > r.h / 2)
        radius = r.h / 2;
    /* Straight edges between the corners. */
    struct rect edges[4] = {
        rect_make(r.x + radius, r.y, r.w - 2 * radius, 1),
        rect_make(r.x + radius, r.y + r.h - 1, r.w - 2 * radius, 1),
        rect_make(r.x, r.y + radius, 1, r.h - 2 * radius),
        rect_make(r.x + r.w - 1, r.y + radius, 1, r.h - 2 * radius),
    };
    for (int i = 0; i < 4; i++)
        draw_blend(s, edges[i], c, alpha);
    if (radius <= 0)
        return;
    for (int32_t j = 0; j < radius; j++) {
        for (int32_t i = 0; i < radius; i++) {
            int32_t outer = (int32_t)corner_cov(i, j, radius, radius);
            int32_t inner = (int32_t)corner_cov(i - 1, j - 1, radius - 1, radius - 1);
            if (i == 0 || j == 0)
                inner = 0;
            int32_t ring = outer - inner;
            if (ring <= 0)
                continue;
            uint32_t a = (uint32_t)ring * alpha / 255;
            draw_pixel_blend(s, r.x + i, r.y + j, c, a);
            draw_pixel_blend(s, r.x + r.w - 1 - i, r.y + j, c, a);
            draw_pixel_blend(s, r.x + i, r.y + r.h - 1 - j, c, a);
            draw_pixel_blend(s, r.x + r.w - 1 - i, r.y + r.h - 1 - j, c, a);
        }
    }
}

void draw_circle(struct surface *s, int32_t cx, int32_t cy, int32_t radius, uint32_t c, uint32_t alpha)
{
    if (radius <= 0)
        return;
    draw_round_rect(s, rect_make(cx - radius, cy - radius, radius * 2, radius * 2), radius, c, alpha, 0xF);
}

static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

void draw_shadow(struct surface *s, struct rect r, int32_t radius, int32_t spread, uint32_t max_alpha)
{
    if (spread <= 0)
        return;
    struct rect outer = rect_inset(r, -spread);
    struct rect cr = rect_intersect(outer, s->clip);
    /* Inner rect where the shadow is fully hidden by the window itself. */
    int32_t ix0 = r.x + radius, ix1 = r.x + r.w - radius;
    for (int32_t y = cr.y; y < cr.y + cr.h; y++) {
        uint32_t *p = px_at(s, 0, y);
        bool inside_rows = y >= r.y && y < r.y + r.h;
        for (int32_t x = cr.x; x < cr.x + cr.w; x++) {
            if (inside_rows && x >= r.x && x < r.x + r.w) {
                /* Skip the interior quickly (except near rounded corners). */
                if (x >= ix0 && x < ix1) {
                    x = ix1 - 1;
                    continue;
                }
            }
            /* Distance from (x, y) to the rounded rectangle r. */
            int32_t qx = x < r.x + radius ? r.x + radius - x : (x >= r.x + r.w - radius ? x - (r.x + r.w - radius - 1) : 0);
            int32_t qy = y < r.y + radius ? r.y + radius - y : (y >= r.y + r.h - radius ? y - (r.y + r.h - radius - 1) : 0);
            int32_t d = (int32_t)isqrt32((uint32_t)(qx * qx + qy * qy) * 16) / 4 - radius;
            if (d <= 0 || d >= spread)
                continue;
            int32_t f = spread - d;
            uint32_t a = (uint32_t)(max_alpha * (uint32_t)(f * f) / (uint32_t)(spread * spread));
            if (a)
                p[x] = color_blend(p[x], 0x000000, a);
        }
    }
}

void blit(struct surface *dst, const struct surface *src, int32_t dx, int32_t dy)
{
    struct rect r = rect_intersect(rect_make(dx, dy, src->w, src->h), dst->clip);
    if (rect_empty(r))
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        const uint32_t *sp = src->px + (size_t)(y - dy) * (size_t)src->stride + (size_t)(r.x - dx);
        memcpy(px_at(dst, r.x, y), sp, (size_t)r.w * 4);
    }
}

void blit_round_bottom(struct surface *dst, const struct surface *src, int32_t dx, int32_t dy, int32_t radius)
{
    if (radius > 32)
        radius = 32;
    if (radius <= 0 || src->h < radius || src->w < 2 * radius) {
        blit(dst, src, dx, dy);
        return;
    }
    const uint8_t (*t)[32] = cov_table(radius);
    struct rect whole = rect_make(dx, dy, src->w, src->h);
    struct rect r = rect_intersect(whole, dst->clip);
    if (rect_empty(r))
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        const uint32_t *sp = src->px + (size_t)(y - dy) * (size_t)src->stride;
        uint32_t *dp = px_at(dst, 0, y);
        if (whole.y + whole.h - 1 - y >= radius) {
            memcpy(dp + r.x, sp + (r.x - dx), (size_t)r.w * 4);
            continue;
        }
        for (int32_t x = r.x; x < r.x + r.w; x++) {
            uint32_t cov = rr_cov(t, whole, radius, 4 | 8, x, y);
            dp[x] = color_blend(dp[x], sp[x - dx], cov);
        }
    }
}

/* ---- Text --------------------------------------------------------------- */

static void draw_glyph(struct surface *s, const struct font_data *f, int32_t x, int32_t y, char ch, uint32_t c)
{
    struct rect g = rect_intersect(rect_make(x, y, f->width, f->height), s->clip);
    if (rect_empty(g))
        return;
    const uint16_t *rows = font_glyph(f, ch);
    for (int32_t yy = g.y; yy < g.y + g.h; yy++) {
        uint16_t bits = rows[yy - y];
        if (!bits)
            continue;
        uint32_t *p = px_at(s, 0, yy);
        for (int32_t xx = g.x; xx < g.x + g.w; xx++) {
            if (bits & (0x8000u >> (xx - x)))
                p[xx] = c;
        }
    }
}

int32_t draw_text_n(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, size_t n, uint32_t c)
{
    int32_t cx = x;
    for (size_t i = 0; i < n && text[i]; i++) {
        if (text[i] != ' ')
            draw_glyph(s, f, cx, y, text[i], c);
        cx += f->width;
    }
    return cx - x;
}

int32_t draw_text(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, uint32_t c)
{
    return draw_text_n(s, f, x, y, text, SIZE_MAX, c);
}

int32_t draw_text_bold(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, uint32_t c)
{
    draw_text(s, f, x + 1, y, text, c);
    return draw_text(s, f, x, y, text, c) + 1;
}

int32_t text_width(const struct font_data *f, const char *text) { return (int32_t)strlen(text) * f->width; }

void draw_text_fit(struct surface *s, const struct font_data *f, int32_t x, int32_t y, const char *text, int32_t max_w, uint32_t c)
{
    int32_t max_chars = max_w / f->width;
    int32_t len = (int32_t)strlen(text);
    if (max_chars <= 0)
        return;
    if (len <= max_chars) {
        draw_text(s, f, x, y, text, c);
        return;
    }
    if (max_chars <= 3) {
        draw_text_n(s, f, x, y, "...", (size_t)max_chars, c);
        return;
    }
    int32_t w = draw_text_n(s, f, x, y, text, (size_t)(max_chars - 3), c);
    draw_text(s, f, x + w, y, "...", c);
}
