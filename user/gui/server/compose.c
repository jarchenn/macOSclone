/* Compositor: scene + cursor layer + scan-out through /dev/display. */
#include "compose.h"
#include "lib.h"
#include <aurora.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CURSOR_BOX_W 16
#define CURSOR_BOX_H 22

static struct surface scene, out;
static int disp = -1, out_fd = -1;
static size_t out_bytes;
static struct rect damage[DAMAGE_MAX];
static int ndamage;
static compose_fn compose;
static cursor_fn draw_cursor;
static int32_t cur_x, cur_y;
static uint64_t frames;
static struct aurora_display_present pending; /* rects to present */

struct surface *compositor_scene(void) { return &scene; }
uint64_t compositor_frames(void) { return frames; }
bool compositor_pending(void) { return ndamage > 0 || pending.n > 0; }

static struct rect cursor_box(int32_t x, int32_t y) { return rect_make(x - 1, y - 1, CURSOR_BOX_W, CURSOR_BOX_H); }

static void free_buffers(void)
{
    free(scene.px);
    scene.px = NULL;
    if (out.px)
        munmap(out.px, out_bytes);
    out.px = NULL;
    if (out_fd >= 0)
        close(out_fd);
    out_fd = -1;
}

static bool alloc_buffers(int32_t w, int32_t h)
{
    size_t bytes;
    if (!surface_size_bytes(w, h, &bytes))
        return false;
    scene.px = malloc(bytes);
    out_fd = aurora_shm_create(bytes, O_CLOEXEC);
    if (!scene.px || out_fd < 0)
        return false;
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, out_fd, 0);
    if (p == MAP_FAILED)
        return false;
    out.px = p;
    out_bytes = bytes;
    scene.w = out.w = w;
    scene.h = out.h = h;
    scene.stride = out.stride = w;
    surface_reset_clip(&scene);
    surface_reset_clip(&out);
    struct aurora_display_attach a = { out_fd, (uint32_t)w, (uint32_t)h, (uint32_t)w };
    return ioctl(disp, AURORA_DISP_ATTACH, &a) == 0;
}

bool compositor_init(int display_fd, int32_t w, int32_t h, compose_fn fn, cursor_fn cfn)
{
    disp = display_fd;
    compose = fn;
    draw_cursor = cfn;
    if (!alloc_buffers(w, h))
        return false;
    compositor_damage_all();
    return true;
}

bool compositor_resize(int32_t w, int32_t h)
{
    free_buffers();
    ndamage = 0;
    pending.n = 0;
    if (!alloc_buffers(w, h))
        return false;
    compositor_damage_all();
    return true;
}

static void add_present(struct rect r)
{
    r = rect_intersect(r, rect_make(0, 0, out.w, out.h));
    if (rect_empty(r))
        return;
    for (uint32_t i = 0; i < pending.n; i++) {
        struct aurora_display_rect *p = &pending.r[i];
        struct rect q = rect_make(p->x, p->y, p->w, p->h);
        if (rect_overlaps(q, r)) {
            q = rect_union(q, r);
            p->x = q.x, p->y = q.y, p->w = q.w, p->h = q.h;
            return;
        }
    }
    if (pending.n == AURORA_DISP_RECTS) {
        struct rect u = r;
        for (uint32_t i = 0; i < pending.n; i++)
            u = rect_union(u, rect_make(pending.r[i].x, pending.r[i].y, pending.r[i].w, pending.r[i].h));
        pending.n = 1;
        pending.r[0] = (struct aurora_display_rect){ u.x, u.y, u.w, u.h };
        return;
    }
    pending.r[pending.n++] = (struct aurora_display_rect){ r.x, r.y, r.w, r.h };
}

void compositor_damage(struct rect r)
{
    r = rect_intersect(r, rect_make(0, 0, scene.w, scene.h));
    if (rect_empty(r))
        return;
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
    struct rect u = r;
    for (int i = 0; i < ndamage; i++)
        u = rect_union(u, damage[i]);
    damage[0] = u;
    ndamage = 1;
}

void compositor_damage_all(void)
{
    damage[0] = rect_make(0, 0, scene.w, scene.h);
    ndamage = 1;
}

/* out = scene in r, with the cursor on top where it overlaps r. */
static void refresh_out(struct rect r)
{
    r = rect_intersect(r, rect_make(0, 0, out.w, out.h));
    if (rect_empty(r))
        return;
    for (int32_t y = r.y; y < r.y + r.h; y++)
        memcpy(out.px + (size_t)y * (size_t)out.stride + (size_t)r.x,
               scene.px + (size_t)y * (size_t)scene.stride + (size_t)r.x, (size_t)r.w * 4);
    struct rect cb = cursor_box(cur_x, cur_y);
    if (rect_overlaps(cb, r)) {
        surface_set_clip(&out, r);
        draw_cursor(&out, cur_x, cur_y);
        surface_reset_clip(&out);
    }
    add_present(r);
}

void compositor_move_cursor(int32_t x, int32_t y)
{
    if (x == cur_x && y == cur_y)
        return;
    struct rect old = cursor_box(cur_x, cur_y);
    cur_x = x;
    cur_y = y;
    refresh_out(old);
    refresh_out(cursor_box(x, y));
}

uint64_t compositor_frame(void)
{
    uint64_t px = 0;
    struct rect list[DAMAGE_MAX];
    int n = ndamage;
    memcpy(list, damage, sizeof(struct rect) * (size_t)n);
    ndamage = 0;
    for (int i = 0; i < n; i++) {
        surface_set_clip(&scene, list[i]);
        compose(&scene, scene.clip);
        surface_reset_clip(&scene);
        refresh_out(list[i]);
        px += (uint64_t)list[i].w * (uint64_t)list[i].h;
    }
    if (pending.n) {
        ioctl(disp, AURORA_DISP_PRESENT, &pending);
        pending.n = 0;
        frames++;
    }
    return px;
}
