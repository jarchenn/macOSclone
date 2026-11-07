/* PS/2 3-byte mouse packet decoder. Portable (host-tested).
 *
 * Byte 0: Yovf Xovf Ysign Xsign 1 Mid Right Left
 * Byte 1: X movement (low 8 bits of a 9-bit two's complement value)
 * Byte 2: Y movement (positive = up)
 */
#include "drivers/input.h"
#include "lib.h"

void mouse_decoder_init(struct mouse_decoder *d)
{
    memset(d, 0, sizeof *d);
    d->size = 3;
}

bool mouse_decode(struct mouse_decoder *d, uint8_t b, uint64_t now_ms, struct mouse_event *out)
{
    if (d->idx > 0 && now_ms - d->last_ms > 50) {
        d->idx = 0; /* stale partial packet: resynchronise */
        d->resyncs++;
    }
    d->last_ms = now_ms;
    if (d->idx == 0 && !(b & 0x08)) {
        d->resyncs++; /* not a valid first byte (bit 3 always set) */
        return false;
    }
    d->pkt[d->idx++] = b;
    uint8_t size = d->size == 4 ? 4 : 3;
    if (d->idx < size)
        return false;
    d->idx = 0;
    int8_t wheel = 0;
    if (size == 4) {
        /* Z movement: low 4 bits, two's complement; + = towards the user. */
        int z = d->pkt[3] & 0x0F;
        if (z & 0x08)
            z -= 16;
        wheel = (int8_t)-z;
    }
    uint8_t s = d->pkt[0];
    int32_t dx = (int32_t)d->pkt[1] - ((s & 0x10) ? 256 : 0);
    int32_t dy = (int32_t)d->pkt[2] - ((s & 0x20) ? 256 : 0);
    if (s & 0xC0) {
        /* Overflow: the magnitude is unreliable; keep buttons, drop motion. */
        d->overflows++;
        dx = dy = 0;
    }
    out->dx = (int16_t)dx;
    out->dy = (int16_t)-dy; /* screen coordinates grow downwards */
    out->buttons = s & 0x07;
    out->wheel = wheel;
    return true;
}
