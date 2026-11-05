#ifndef AURORA_COMPOSITOR_H
#define AURORA_COMPOSITOR_H
#include "graphics/surface.h"

/* Software compositor: maintains a screen-sized XRGB backbuffer and a
 * bounded list of damaged rectangles. Each frame recomposes only damaged
 * areas (wallpaper -> windows -> chrome -> cursor) and presents them. */
#define DAMAGE_MAX 16

typedef void (*compose_fn)(struct surface *back, struct rect clip);

bool compositor_init(int32_t w, int32_t h, compose_fn fn);
void compositor_damage(struct rect r);
void compositor_damage_all(void);
bool compositor_pending(void);
/* Composes and presents all damage. Returns the number of pixels presented. */
uint64_t compositor_frame(void);
struct surface *compositor_backbuffer(void);
uint64_t compositor_frames(void);

#endif
