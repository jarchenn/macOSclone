#ifndef AURORA_EVENT_QUEUE_H
#define AURORA_EVENT_QUEUE_H
#include "types.h"

/* Single-producer / single-consumer byte ring used between IRQ handlers
 * (producer) and the main loop (consumer) on a single CPU.
 *
 * Synchronisation: the producer writes the slot, then publishes `head` with a
 * release store; the consumer reads `head` with an acquire load. Each index
 * has exactly one writer, so no lock is needed. Overflow policy: when full,
 * the NEW byte is dropped and `dropped` is incremented (older bytes are kept
 * so multi-byte packets already queued stay intact). Capacity must be a power
 * of two. */
#define BYTE_RING_CAP 512

struct byte_ring {
    uint8_t buf[BYTE_RING_CAP];
    uint32_t head; /* written by producer only */
    uint32_t tail; /* written by consumer only */
    uint32_t dropped;
};

bool byte_ring_push(struct byte_ring *r, uint8_t b);
bool byte_ring_pop(struct byte_ring *r, uint8_t *out);
uint32_t byte_ring_count(const struct byte_ring *r);

/* Decoded input events, main-loop context only. Bounded FIFO. Overflow
 * policy: consecutive mouse-motion events with identical button state are
 * coalesced; if the queue is still full the new event is dropped and counted. */
enum event_type { EV_NONE = 0, EV_KEY, EV_MOUSE };

struct key_event {
    uint16_t key;    /* enum keycode */
    uint8_t ascii;   /* printable character after modifiers, or 0 */
    uint8_t mods;    /* MOD_* bitmask */
    bool pressed;
};

struct mouse_event {
    int16_t dx, dy;  /* screen-oriented deltas (dy > 0 = down) */
    uint8_t buttons; /* bit0 left, bit1 right, bit2 middle */
    int8_t wheel;    /* 4-byte (IntelliMouse) packets: + = scroll up */
};

struct event {
    uint8_t type;
    union {
        struct key_event key;
        struct mouse_event mouse;
    };
};

#define EVENT_QUEUE_CAP 256
struct event_queue {
    struct event ev[EVENT_QUEUE_CAP];
    uint32_t head, count;
    uint32_t dropped, coalesced;
};

void event_queue_init(struct event_queue *q);
bool event_queue_push(struct event_queue *q, const struct event *e);
bool event_queue_pop(struct event_queue *q, struct event *out);

#endif
