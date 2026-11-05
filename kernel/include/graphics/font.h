#ifndef AURORA_FONT_H
#define AURORA_FONT_H
#include <stdint.h>

/* Monospaced bitmap font covering ASCII 32..126. Each glyph row is a 16-bit
 * value with the leftmost pixel in bit 15. */
struct font_data {
    uint8_t width;
    uint8_t height;
    uint16_t glyphs[95][24];
};

extern const struct font_data font_spleen_8x16;
extern const struct font_data font_spleen_12x24;

/* sin(2*pi*i/256) * 4096, generated at build time. */
extern const int16_t sine_table_q12[256];

static inline const uint16_t *font_glyph(const struct font_data *f, char c)
{
    unsigned char u = (unsigned char)c;
    if (u < 32 || u > 126)
        u = '?';
    return f->glyphs[u - 32];
}

#endif
