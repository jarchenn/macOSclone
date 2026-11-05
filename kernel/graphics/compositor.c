#include "graphics/compositor.h"
#include "drivers/framebuffer.h"
#include "lib.h"

static struct surface back;
static struct rect damage[DAMAGE_MAX];
static int ndamage;
static compose_fn compose;
static uint64_t frames;

bool compositor_init(int32_t w, int32_t h, compose_fn fn)
{
    if (!surface_alloc(&back, w, h))
        return false;
    compose = fn;
    ndamage = 0;
    compositor_damage_all();
    return true;
}

struct surface *compositor_backbuffer(void) { return &back; }
uint64_t compositor_frames(void) { return frames; }
bool compositor_pending(void) { return ndamage > 0; }

void compositor_damage(struct rect r)
{
    r = rect_intersect(r, rect_make(0, 0, back.w, back.h));
    if (rect_empty(r))
        return;
    /* Merge with an overlapping rect when that does not waste much area. */
    for (int i = 0; i < ndamage; i++) {
        struct rect u = rect_union(damage[i], r);
        int64_t ua = (int64_t)u.w * u.h;
        int64_t sum = (int64_t)damage[i].w * damage[i].h + (int64_t)r.w * r.h;
        if (rect_overlaps(damage[i], r) || ua <= sum + sum / 4) {
            damage[i] = u;
            return;
        }
    }
    if (ndamage < DAMAGE_MAX) {
        damage[ndamage++] = r;
        return;
    }
    /* List full: collapse everything into one bounding rectangle. */
    struct rect u = r;
    for (int i = 0; i < ndamage; i++)
        u = rect_union(u, damage[i]);
    damage[0] = u;
    ndamage = 1;
}

void compositor_damage_all(void)
{
    damage[0] = rect_make(0, 0, back.w, back.h);
    ndamage = 1;
}

uint64_t compositor_frame(void)
{
    if (!ndamage)
        return 0;
    /* Copy the list: compose callbacks must not add damage mid-frame, but be
     * defensive anyway. */
    struct rect list[DAMAGE_MAX];
    int n = ndamage;
    memcpy(list, damage, sizeof(struct rect) * (size_t)n);
    ndamage = 0;
    uint64_t px = 0;
    for (int i = 0; i < n; i++) {
        surface_set_clip(&back, list[i]);
        compose(&back, back.clip);
        surface_reset_clip(&back);
    }
    for (int i = 0; i < n; i++) {
        fb_present(&back, list[i]);
        px += (uint64_t)list[i].w * (uint64_t)list[i].h;
    }
    frames++;
    return px;
}
