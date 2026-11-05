#include "core/event_queue.h"
#include "lib.h"

_Static_assert((BYTE_RING_CAP & (BYTE_RING_CAP - 1)) == 0, "ring capacity must be a power of two");

bool byte_ring_push(struct byte_ring *r, uint8_t b)
{
    uint32_t head = r->head;
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    if (head - tail >= BYTE_RING_CAP) {
        r->dropped++;
        return false;
    }
    r->buf[head & (BYTE_RING_CAP - 1)] = b;
    __atomic_store_n(&r->head, head + 1, __ATOMIC_RELEASE);
    return true;
}

bool byte_ring_pop(struct byte_ring *r, uint8_t *out)
{
    uint32_t tail = r->tail;
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    if (head == tail)
        return false;
    *out = r->buf[tail & (BYTE_RING_CAP - 1)];
    __atomic_store_n(&r->tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

uint32_t byte_ring_count(const struct byte_ring *r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE) - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

void event_queue_init(struct event_queue *q) { memset(q, 0, sizeof *q); }

bool event_queue_push(struct event_queue *q, const struct event *e)
{
    if (e->type == EV_MOUSE && q->count > 0) {
        struct event *last = &q->ev[(q->head + q->count - 1) % EVENT_QUEUE_CAP];
        if (last->type == EV_MOUSE && last->mouse.buttons == e->mouse.buttons) {
            int32_t dx = last->mouse.dx + e->mouse.dx, dy = last->mouse.dy + e->mouse.dy;
            if (dx >= INT16_MIN && dx <= INT16_MAX && dy >= INT16_MIN && dy <= INT16_MAX) {
                last->mouse.dx = (int16_t)dx;
                last->mouse.dy = (int16_t)dy;
                q->coalesced++;
                return true;
            }
        }
    }
    if (q->count >= EVENT_QUEUE_CAP) {
        q->dropped++;
        return false;
    }
    q->ev[(q->head + q->count) % EVENT_QUEUE_CAP] = *e;
    q->count++;
    return true;
}

bool event_queue_pop(struct event_queue *q, struct event *out)
{
    if (q->count == 0)
        return false;
    *out = q->ev[q->head];
    q->head = (q->head + 1) % EVENT_QUEUE_CAP;
    q->count--;
    return true;
}
