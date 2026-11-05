#ifndef AURORA_RECT_H
#define AURORA_RECT_H
#include "types.h"

struct rect {
    int32_t x, y, w, h;
};

static inline struct rect rect_make(int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct rect r = { x, y, w, h };
    return r;
}
static inline bool rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }

/* All rect math is done in int64 internally so huge or negative inputs
 * cannot overflow; results are clamped back into int32 range. */
struct rect rect_intersect(struct rect a, struct rect b);
struct rect rect_union(struct rect a, struct rect b);
bool rect_contains(struct rect r, int32_t x, int32_t y);
bool rect_overlaps(struct rect a, struct rect b);
struct rect rect_inset(struct rect r, int32_t d);

/* Byte offset of pixel (x, y) in a linear framebuffer, computed from the
 * reported pitch (never width * bpp). Returns false if the pixel lies outside
 * width x height or the arithmetic would overflow buf_size. */
bool fb_pixel_offset(uint64_t pitch, uint32_t bpp, uint64_t width, uint64_t height,
                     uint64_t buf_size, int64_t x, int64_t y, uint64_t *out);

/* Checked size of a w*h 32-bit surface; returns false on overflow or
 * dimensions outside 1..16384. */
bool surface_size_bytes(int64_t w, int64_t h, size_t *out);

#endif
