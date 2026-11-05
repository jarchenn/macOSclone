#include "graphics/rect.h"

static int32_t clamp32(int64_t v)
{
    if (v > INT32_MAX)
        return INT32_MAX;
    if (v < INT32_MIN)
        return INT32_MIN;
    return (int32_t)v;
}

struct rect rect_intersect(struct rect a, struct rect b)
{
    struct rect r = { 0, 0, 0, 0 };
    if (rect_empty(a) || rect_empty(b))
        return r;
    int64_t x0 = MAX((int64_t)a.x, (int64_t)b.x);
    int64_t y0 = MAX((int64_t)a.y, (int64_t)b.y);
    int64_t x1 = MIN((int64_t)a.x + a.w, (int64_t)b.x + b.w);
    int64_t y1 = MIN((int64_t)a.y + a.h, (int64_t)b.y + b.h);
    if (x1 <= x0 || y1 <= y0)
        return r;
    r.x = clamp32(x0);
    r.y = clamp32(y0);
    r.w = clamp32(x1 - x0);
    r.h = clamp32(y1 - y0);
    return r;
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (rect_empty(a))
        return b;
    if (rect_empty(b))
        return a;
    int64_t x0 = MIN((int64_t)a.x, (int64_t)b.x);
    int64_t y0 = MIN((int64_t)a.y, (int64_t)b.y);
    int64_t x1 = MAX((int64_t)a.x + a.w, (int64_t)b.x + b.w);
    int64_t y1 = MAX((int64_t)a.y + a.h, (int64_t)b.y + b.h);
    struct rect r = { clamp32(x0), clamp32(y0), clamp32(x1 - x0), clamp32(y1 - y0) };
    return r;
}

bool rect_contains(struct rect r, int32_t x, int32_t y)
{
    return !rect_empty(r) && (int64_t)x >= r.x && (int64_t)y >= r.y &&
           (int64_t)x < (int64_t)r.x + r.w && (int64_t)y < (int64_t)r.y + r.h;
}

bool rect_overlaps(struct rect a, struct rect b) { return !rect_empty(rect_intersect(a, b)); }

struct rect rect_inset(struct rect r, int32_t d)
{
    int64_t w = (int64_t)r.w - 2 * (int64_t)d, h = (int64_t)r.h - 2 * (int64_t)d;
    struct rect o = { clamp32((int64_t)r.x + d), clamp32((int64_t)r.y + d), clamp32(w < 0 ? 0 : w), clamp32(h < 0 ? 0 : h) };
    return o;
}

bool fb_pixel_offset(uint64_t pitch, uint32_t bpp, uint64_t width, uint64_t height,
                     uint64_t buf_size, int64_t x, int64_t y, uint64_t *out)
{
    if (x < 0 || y < 0 || (uint64_t)x >= width || (uint64_t)y >= height)
        return false;
    if (bpp == 0 || bpp % 8 != 0 || bpp > 32)
        return false;
    uint64_t bytes_pp = bpp / 8;
    if (pitch < width * bytes_pp)
        return false;
    /* y * pitch overflow check */
    if (pitch != 0 && (uint64_t)y > UINT64_MAX / pitch)
        return false;
    uint64_t off = (uint64_t)y * pitch + (uint64_t)x * bytes_pp;
    if (off + bytes_pp > buf_size || off + bytes_pp < off)
        return false;
    *out = off;
    return true;
}

bool surface_size_bytes(int64_t w, int64_t h, size_t *out)
{
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384)
        return false;
    uint64_t n = (uint64_t)w * (uint64_t)h;
    if (n > SIZE_MAX / 4)
        return false;
    *out = (size_t)(n * 4);
    return true;
}
