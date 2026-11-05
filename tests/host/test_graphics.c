/* Rectangle clipping, framebuffer offsets with unusual pitch, size overflow. */
#include "harness.h"
#include "graphics/rect.h"

const char *t_suite = "graphics";

TEST(intersect_basic)
{
    struct rect r = rect_intersect(rect_make(0, 0, 100, 100), rect_make(50, 60, 100, 100));
    CHECK_EQ(r.x, 50);
    CHECK_EQ(r.y, 60);
    CHECK_EQ(r.w, 50);
    CHECK_EQ(r.h, 40);
    CHECK(rect_empty(rect_intersect(rect_make(0, 0, 10, 10), rect_make(10, 0, 10, 10)))); /* touching edges */
    CHECK(rect_empty(rect_intersect(rect_make(0, 0, -5, 10), rect_make(0, 0, 10, 10))));  /* negative size */
}

TEST(intersect_negative_coords)
{
    struct rect r = rect_intersect(rect_make(-50, -50, 100, 100), rect_make(0, 0, 1024, 768));
    CHECK_EQ(r.x, 0);
    CHECK_EQ(r.y, 0);
    CHECK_EQ(r.w, 50);
    CHECK_EQ(r.h, 50);
}

TEST(no_overflow_near_int_max)
{
    struct rect big = rect_make(INT32_MAX - 10, INT32_MAX - 10, INT32_MAX, INT32_MAX);
    struct rect r = rect_intersect(big, rect_make(0, 0, INT32_MAX, INT32_MAX));
    CHECK(!rect_empty(r));
    CHECK(r.w <= 10 && r.w > 0);
    struct rect u = rect_union(rect_make(INT32_MIN, 0, 10, 10), rect_make(INT32_MAX - 5, 0, 5, 10));
    CHECK(u.w == INT32_MAX); /* clamped instead of wrapping negative */
    CHECK(!rect_contains(rect_make(INT32_MAX - 1, 0, INT32_MAX, 1), INT32_MIN, 0));
}

TEST(union_and_contains)
{
    struct rect u = rect_union(rect_make(10, 10, 5, 5), rect_make(30, 40, 5, 5));
    CHECK_EQ(u.x, 10);
    CHECK_EQ(u.y, 10);
    CHECK_EQ(u.w, 25);
    CHECK_EQ(u.h, 35);
    CHECK(rect_contains(u, 10, 10));
    CHECK(!rect_contains(u, 35, 45)); /* exclusive right/bottom edge */
    struct rect e = rect_union(rect_make(0, 0, 0, 0), rect_make(3, 4, 5, 6));
    CHECK_EQ(e.x, 3);
    CHECK_EQ(e.w, 5);
}

TEST(fb_offset_unusual_pitch)
{
    uint64_t off;
    /* 1023 wide, 32bpp, pitch padded to 4160 bytes */
    CHECK(fb_pixel_offset(4160, 32, 1023, 768, 4160ull * 768, 0, 1, &off));
    CHECK_EQ(off, 4160);
    CHECK(fb_pixel_offset(4160, 32, 1023, 768, 4160ull * 768, 1022, 767, &off));
    CHECK_EQ(off, 4160ull * 767 + 1022 * 4);
    CHECK(!fb_pixel_offset(4160, 32, 1023, 768, 4160ull * 768, 1023, 0, &off)); /* x == width */
    CHECK(!fb_pixel_offset(4160, 32, 1023, 768, 4160ull * 768, -1, 0, &off));
    CHECK(!fb_pixel_offset(4160, 32, 1023, 768, 4160ull * 768, 0, 768, &off));
    /* 24 bpp with odd pitch */
    CHECK(fb_pixel_offset(3001, 24, 1000, 10, 3001ull * 10, 999, 9, &off));
    CHECK_EQ(off, 3001ull * 9 + 999 * 3);
    /* pitch smaller than width*bpp is rejected */
    CHECK(!fb_pixel_offset(100, 32, 1024, 10, 1 << 20, 0, 0, &off));
    /* buffer too small for the computed offset */
    CHECK(!fb_pixel_offset(4096, 32, 1024, 768, 4096, 0, 1, &off));
    /* unsupported bpp */
    CHECK(!fb_pixel_offset(4096, 15, 1024, 768, 1 << 22, 0, 0, &off));
    /* y * pitch overflow */
    CHECK(!fb_pixel_offset(UINT64_MAX / 2, 32, 4, UINT64_MAX, UINT64_MAX, 0, 4, &off));
}

TEST(surface_size_checks)
{
    size_t n;
    CHECK(surface_size_bytes(1024, 768, &n));
    CHECK_EQ(n, 1024 * 768 * 4);
    CHECK(!surface_size_bytes(0, 10, &n));
    CHECK(!surface_size_bytes(-1, 10, &n));
    CHECK(!surface_size_bytes(100000, 10, &n));
    CHECK(!surface_size_bytes(10, 16385, &n));
    CHECK(surface_size_bytes(16384, 16384, &n));
}

void run_tests(void)
{
    RUN(intersect_basic);
    RUN(intersect_negative_coords);
    RUN(no_overflow_near_int_max);
    RUN(union_and_contains);
    RUN(fb_offset_unusual_pitch);
    RUN(surface_size_checks);
}
