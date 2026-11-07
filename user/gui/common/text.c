/* TrueType text with a glyph cache, gamma-correct blending and kerning
 * (gui/text.h). */
#include "gui/text.h"
#include "graphics/font.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_assert(x) ((void)0)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#include "stb_truetype.h"
#pragma clang diagnostic pop

#define CACHE_SLOTS 512 /* per font; open addressing on the code point */

struct glyph {
    uint32_t cp;      /* 0 = empty slot */
    int gi;           /* glyph index in the face */
    int16_t x0, y0;   /* bitmap offset from the pen (y from the baseline) */
    int16_t w, h;
    float adv;        /* advance in pixels */
    uint8_t *alpha;   /* w * h coverage */
};

struct face {
    const char *path;
    bool tried, ok;
    uint8_t *data;
    stbtt_fontinfo info;
};

struct text_font {
    int face;
    int px;
    bool bitmap;
    const struct font_data *bmp;
    float scale;
    int32_t ascent, height, cell;
    struct glyph *cache;
};

static struct face faces[3] = {
    { "/system/fonts/DejaVuSans.ttf", false, false, NULL, { 0 } },
    { "/system/fonts/DejaVuSans-Bold.ttf", false, false, NULL, { 0 } },
    { "/system/fonts/DejaVuSansMono.ttf", false, false, NULL, { 0 } },
};

static struct text_font f_ui = { .face = 0, .px = 13 }, f_bold = { .face = 1, .px = 13 },
                        f_small = { .face = 0, .px = 11 }, f_title = { .face = 1, .px = 20 },
                        f_mono = { .face = 2, .px = 15 };
struct text_font *const FONT_UI = &f_ui;
struct text_font *const FONT_UI_BOLD = &f_bold;
struct text_font *const FONT_SMALL = &f_small;
struct text_font *const FONT_TITLE = &f_title;
struct text_font *const FONT_MONO = &f_mono;

static struct text_font extra[24];
static int nextra;
static uint64_t st_hits, st_misses, st_glyphs;

/* ---- gamma ------------------------------------------------------------------ */

static uint16_t to_lin[256];    /* sRGB byte -> linear 0..4095 */
static uint8_t to_srgb[4096];   /* linear -> sRGB byte */
static bool gamma_ready;

static void gamma_init(void)
{
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        double l = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
        to_lin[i] = (uint16_t)(l * 4095.0 + 0.5);
    }
    for (int i = 0; i < 4096; i++) {
        double l = i / 4095.0;
        double c = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        int v = (int)(c * 255.0 + 0.5);
        to_srgb[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    gamma_ready = true;
}

static inline uint32_t blend_lin(uint32_t dst, uint32_t src, uint32_t a)
{
    if (a >= 255)
        return src;
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t d = to_lin[(dst >> sh) & 0xFF], s = to_lin[(src >> sh) & 0xFF];
        uint32_t l = (s * a + d * (255 - a)) / 255;
        out |= (uint32_t)to_srgb[l] << sh;
    }
    return out;
}

/* ---- loading ------------------------------------------------------------------ */

static bool load_face(int i)
{
    struct face *fc = &faces[i];
    if (fc->tried)
        return fc->ok;
    fc->tried = true;
    FILE *f = fopen(fc->path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > (16l << 20)) {
        fclose(f);
        return false;
    }
    fc->data = malloc((size_t)n);
    if (!fc->data || fread(fc->data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(fc->data);
        fc->data = NULL;
        return false;
    }
    fclose(f);
    if (!stbtt_InitFont(&fc->info, fc->data, stbtt_GetFontOffsetForIndex(fc->data, 0))) {
        free(fc->data);
        fc->data = NULL;
        return false;
    }
    fc->ok = true;
    return true;
}

static void setup(struct text_font *f)
{
    if (f->cache || f->bitmap)
        return;
    if (!gamma_ready)
        gamma_init();
    if (load_face(f->face)) {
        stbtt_fontinfo *info = &faces[f->face].info;
        int asc, desc, gap;
        stbtt_GetFontVMetrics(info, &asc, &desc, &gap);
        f->scale = stbtt_ScaleForPixelHeight(info, (float)f->px);
        /* Line box: ascent + descent, rounded up, plus a little air. */
        f->ascent = (int32_t)ceilf(asc * f->scale);
        int32_t descent = (int32_t)ceilf(-desc * f->scale);
        f->height = f->ascent + descent + (f->px >= 16 ? 2 : 1);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(info, 'n', &adv, &lsb);
        f->cell = (int32_t)lroundf(adv * f->scale);
        if (f->face == 2) {
            stbtt_GetCodepointHMetrics(info, 'M', &adv, &lsb);
            f->cell = (int32_t)lroundf(adv * f->scale);
        }
        f->cache = calloc(CACHE_SLOTS, sizeof(struct glyph));
        if (f->cache)
            return;
    }
    /* Fallback: the bitmap font nearest in size. */
    f->bitmap = true;
    f->bmp = f->px >= 18 ? &font_spleen_12x24 : &font_spleen_8x16;
    f->ascent = f->bmp->height - 3;
    f->height = f->bmp->height;
    f->cell = f->bmp->width;
}

bool text_init(void)
{
    struct text_font *all[] = { FONT_UI, FONT_UI_BOLD, FONT_SMALL, FONT_TITLE, FONT_MONO };
    bool ok = true;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        setup(all[i]);
        ok &= !all[i]->bitmap;
    }
    return ok;
}

struct text_font *text_font_at(int face, int px)
{
    if (face < 0 || face > 2)
        face = 0;
    if (px < 6)
        px = 6;
    if (px > 96)
        px = 96;
    struct text_font *std[] = { FONT_UI, FONT_UI_BOLD, FONT_SMALL, FONT_TITLE, FONT_MONO };
    for (size_t i = 0; i < sizeof std / sizeof std[0]; i++)
        if (std[i]->face == face && std[i]->px == px) {
            setup(std[i]);
            return std[i];
        }
    for (int i = 0; i < nextra; i++)
        if (extra[i].face == face && extra[i].px == px)
            return &extra[i];
    if (nextra >= (int)(sizeof extra / sizeof extra[0]))
        return face == 2 ? FONT_MONO : FONT_UI;
    struct text_font *f = &extra[nextra++];
    memset(f, 0, sizeof *f);
    f->face = face;
    f->px = px;
    setup(f);
    return f;
}

bool text_font_is_bitmap(const struct text_font *f)
{
    setup((struct text_font *)f);
    return f->bitmap;
}

int32_t text_height(const struct text_font *f)
{
    setup((struct text_font *)f);
    return f->height;
}

int32_t text_ascent(const struct text_font *f)
{
    setup((struct text_font *)f);
    return f->ascent;
}

int32_t text_cell_width(const struct text_font *f)
{
    setup((struct text_font *)f);
    return f->cell;
}

/* ---- UTF-8 ------------------------------------------------------------------------ */

uint32_t utf8_decode(const char *s, size_t n, size_t *len)
{
    const uint8_t *u = (const uint8_t *)s;
    *len = 1;
    if (n == 0)
        return 0xFFFD;
    uint8_t b = u[0];
    if (b < 0x80)
        return b;
    int need;
    uint32_t cp, min;
    if ((b & 0xE0) == 0xC0) {
        need = 1;
        cp = b & 0x1F;
        min = 0x80;
    } else if ((b & 0xF0) == 0xE0) {
        need = 2;
        cp = b & 0x0F;
        min = 0x800;
    } else if ((b & 0xF8) == 0xF0) {
        need = 3;
        cp = b & 0x07;
        min = 0x10000;
    } else {
        return 0xFFFD;
    }
    if ((size_t)need >= n)
        return 0xFFFD;
    for (int i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (u[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return 0xFFFD;
    *len = (size_t)need + 1;
    return cp;
}

size_t utf8_encode(uint32_t cp, char out[4])
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf8_prev(const char *s, size_t i)
{
    if (i == 0)
        return 0;
    size_t k = i - 1;
    while (k > 0 && i - k < 4 && ((uint8_t)s[k] & 0xC0) == 0x80)
        k--;
    return k;
}

size_t utf8_next(const char *s, size_t n, size_t i)
{
    if (i >= n)
        return n;
    size_t len;
    utf8_decode(s + i, n - i, &len);
    return i + len;
}

/* ---- glyphs ------------------------------------------------------------------------ */

static struct glyph *glyph(struct text_font *f, uint32_t cp)
{
    uint32_t h = (cp * 2654435761u) % CACHE_SLOTS;
    for (int probe = 0; probe < CACHE_SLOTS; probe++) {
        struct glyph *g = &f->cache[(h + (uint32_t)probe) % CACHE_SLOTS];
        if (g->cp == cp) {
            st_hits++;
            return g;
        }
        if (g->cp == 0) {
            st_misses++;
            stbtt_fontinfo *info = &faces[f->face].info;
            g->cp = cp;
            g->gi = stbtt_FindGlyphIndex(info, (int)cp);
            int adv, lsb;
            stbtt_GetGlyphHMetrics(info, g->gi, &adv, &lsb);
            g->adv = adv * f->scale;
            int x0, y0, x1, y1;
            stbtt_GetGlyphBitmapBox(info, g->gi, f->scale, f->scale, &x0, &y0, &x1, &y1);
            g->x0 = (int16_t)x0;
            g->y0 = (int16_t)y0;
            g->w = (int16_t)(x1 - x0);
            g->h = (int16_t)(y1 - y0);
            if (g->w > 0 && g->h > 0) {
                g->alpha = malloc((size_t)g->w * (size_t)g->h);
                if (g->alpha) {
                    stbtt_MakeGlyphBitmap(info, g->alpha, g->w, g->h, g->w, f->scale, f->scale, g->gi);
                    st_glyphs++;
                }
            }
            return g;
        }
    }
    /* Cache full (more than CACHE_SLOTS distinct code points): evict all. */
    for (int i = 0; i < CACHE_SLOTS; i++) {
        free(f->cache[i].alpha);
        f->cache[i] = (struct glyph){ 0 };
    }
    return glyph(f, cp);
}

static void blit_glyph(struct surface *s, const struct glyph *g, int32_t x, int32_t base, uint32_t c)
{
    if (!g->alpha || !s)
        return;
    int32_t gx = x + g->x0, gy = base + g->y0;
    struct rect r = rect_intersect(s->clip, rect_make(gx, gy, g->w, g->h));
    for (int32_t y = r.y; y < r.y + r.h; y++) {
        const uint8_t *a = g->alpha + (size_t)(y - gy) * (size_t)g->w + (size_t)(r.x - gx);
        uint32_t *p = s->px + (size_t)y * (size_t)s->stride + (size_t)r.x;
        for (int32_t i = 0; i < r.w; i++)
            if (a[i])
                p[i] = blend_lin(p[i], c, a[i]);
    }
}

/* Draws or measures; returns the advance. */
static int32_t run(struct surface *s, const struct text_font *cf, int32_t x, int32_t y, const char *t, size_t n,
                   uint32_t c)
{
    struct text_font *f = (struct text_font *)cf;
    setup(f);
    if (f->bitmap) {
        /* Bitmap fallback: ASCII only, one cell per code point. */
        int32_t adv = 0;
        for (size_t i = 0; i < n;) {
            size_t len;
            uint32_t cp = utf8_decode(t + i, n - i, &len);
            char ch = cp >= 32 && cp < 127 ? (char)cp : '?';
            if (s)
                draw_text_n(s, f->bmp, x + adv, y, &ch, 1, c);
            adv += f->bmp->width;
            i += len;
        }
        return adv;
    }
    stbtt_fontinfo *info = &faces[f->face].info;
    float pen = 0;
    int prev = -1;
    int32_t base = y + f->ascent;
    for (size_t i = 0; i < n;) {
        size_t len;
        uint32_t cp = utf8_decode(t + i, n - i, &len);
        i += len;
        if (cp == '\t')
            cp = ' ';
        if (cp < 32)
            continue;
        struct glyph *g = glyph(f, cp);
        if (prev >= 0 && f->face != 2)
            pen += stbtt_GetGlyphKernAdvance(info, prev, g->gi) * f->scale;
        if (s)
            blit_glyph(s, g, x + (int32_t)lroundf(pen), base, c);
        pen += f->face == 2 ? (float)f->cell : g->adv;
        prev = g->gi;
    }
    return (int32_t)lroundf(pen);
}

int32_t text_draw(struct surface *s, const struct text_font *f, int32_t x, int32_t y, const char *t, uint32_t c)
{
    return run(s, f, x, y, t, strlen(t), c);
}

int32_t text_draw_n(struct surface *s, const struct text_font *f, int32_t x, int32_t y, const char *t, size_t n,
                    uint32_t c)
{
    return run(s, f, x, y, t, n, c);
}

int32_t text_draw_cp(struct surface *s, const struct text_font *f, int32_t x, int32_t y, uint32_t cp, uint32_t c)
{
    char b[4];
    size_t n = utf8_encode(cp, b);
    return run(s, f, x, y, b, n, c);
}

int32_t text_width(const struct text_font *f, const char *t) { return run(NULL, f, 0, 0, t, strlen(t), 0); }
int32_t text_width_n(const struct text_font *f, const char *t, size_t n) { return run(NULL, f, 0, 0, t, n, 0); }

void text_draw_fit(struct surface *s, const struct text_font *f, int32_t x, int32_t y, const char *t, int32_t max_w,
                   uint32_t c)
{
    size_t n = strlen(t);
    if (text_width_n(f, t, n) <= max_w) {
        run(s, f, x, y, t, n, c);
        return;
    }
    const char *ell = "\xE2\x80\xA6"; /* U+2026 */
    int32_t ew = text_width(f, ell);
    size_t k = text_fit(f, t, n, max_w - ew, false);
    int32_t w = run(s, f, x, y, t, k, c);
    run(s, f, x + w, y, ell, 3, c);
}

size_t text_fit(const struct text_font *f, const char *t, size_t n, int32_t max_w, bool words)
{
    size_t last_space = 0, i = 0;
    while (i < n) {
        size_t next = utf8_next(t, n, i);
        if (text_width_n(f, t, next) > max_w)
            break;
        if (t[i] == ' ')
            last_space = next;
        i = next;
    }
    if (i < n && words && last_space)
        return last_space;
    return i;
}

int32_t text_offset(const struct text_font *f, const char *t, size_t n, size_t i)
{
    return text_width_n(f, t, i < n ? i : n);
}

size_t text_index_at(const struct text_font *f, const char *t, size_t n, int32_t x)
{
    size_t i = 0;
    int32_t prev = 0;
    while (i < n) {
        size_t next = utf8_next(t, n, i);
        int32_t w = text_width_n(f, t, next);
        if (x < (prev + w) / 2)
            return i;
        prev = w;
        i = next;
    }
    return n;
}

int32_t text_wrap(struct surface *s, const struct text_font *f, int32_t x, int32_t y, int32_t w, int32_t line_h,
                  const char *t, uint32_t c)
{
    while (*t) {
        const char *nl = strchr(t, '\n');
        size_t len = nl ? (size_t)(nl - t) : strlen(t);
        size_t off = 0;
        if (len == 0)
            y += line_h;
        while (off < len) {
            size_t k = text_fit(f, t + off, len - off, w, true);
            if (k == 0)
                k = utf8_next(t + off, len - off, 0); /* one code point even if too wide */
            size_t draw = k;
            while (draw > 0 && t[off + draw - 1] == ' ')
                draw--;
            if (s)
                run(s, f, x, y, t + off, draw, c);
            y += line_h;
            off += k;
            while (off < len && t[off] == ' ')
                off++;
        }
        if (!nl)
            break;
        t = nl + 1;
    }
    return y;
}

void text_stats(uint64_t *hits, uint64_t *misses, uint64_t *glyphs)
{
    *hits = st_hits;
    *misses = st_misses;
    *glyphs = st_glyphs;
}
