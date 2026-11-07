#ifndef AURORA_SERVER_COMPOSE_H
#define AURORA_SERVER_COMPOSE_H
#include "gui/desktop.h"

/* Software compositor (0.7a). Two screen-sized XRGB8888 buffers:
 *   scene  - wallpaper, windows, menu bar, dock and menus (private)
 *   out    - scene plus the cursor; a shared-memory object attached to
 *            /dev/display, from which the kernel scans damaged rectangles out.
 * The cursor is a separate layer: moving it copies two small rectangles
 * from scene to out and redraws it, without recomposing anything. Damage
 * is a bounded list of rectangles (merged when overlapping). */
#define DAMAGE_MAX 16

typedef void (*compose_fn)(struct surface *scene, struct rect clip);
typedef void (*cursor_fn)(struct surface *out, int32_t x, int32_t y);

bool compositor_init(int display_fd, int32_t w, int32_t h, compose_fn compose, cursor_fn cursor);
bool compositor_resize(int32_t w, int32_t h);
void compositor_damage(struct rect r);
void compositor_damage_all(void);
bool compositor_pending(void);
/* The cursor's bounding box moved from `old` to `now` (both screen rects). */
void compositor_move_cursor(int32_t x, int32_t y);
/* Composes and presents all damage; returns the pixels presented. */
uint64_t compositor_frame(void);
uint64_t compositor_frames(void);
struct surface *compositor_scene(void);

#endif
