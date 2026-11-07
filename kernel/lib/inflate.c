/* DEFLATE decoder (lib/inflate.h). Canonical Huffman codes are decoded one
 * bit at a time against per-length code counts (RFC 1951 section 3.2.2),
 * which needs no lookup tables: small and easy to check, fast enough for a
 * few megabytes once at boot. */
#include "lib/inflate.h"
#include "lib.h"

#define MAXBITS 15
#define NLITLEN 288
#define NDIST 30

struct state {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *out;
    size_t cap, out_pos;
    bool err;
};

struct huff {
    uint16_t count[MAXBITS + 1]; /* codes of each length */
    uint16_t symbol[NLITLEN];    /* symbols ordered by code */
};

static uint32_t bits(struct state *s, int need)
{
    uint32_t v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) {
            s->err = true;
            return 0;
        }
        v |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return v & ((1u << need) - 1);
}

/* Builds the canonical code; returns 0, or < 0 for an over-subscribed set
 * (> 0: incomplete, allowed only for single-code distance sets). */
static int build(struct huff *h, const uint8_t *lengths, int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    if (h->count[0] == n)
        return 0; /* no codes */
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return -1;
    }
    uint16_t offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return left;
}

static int decode(struct state *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)bits(s, 1);
        if (s->err)
            return -1;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = true;
    return -1;
}

static const uint16_t len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                        513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(struct state *s, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        int sym = decode(s, lit);
        if (s->err || sym < 0)
            return INFLATE_ETRUNC;
        if (sym < 256) {
            if (s->out_pos >= s->cap)
                return INFLATE_EFULL;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {
            return INFLATE_OK;
        } else {
            sym -= 257;
            if (sym >= 29)
                return INFLATE_EDATA;
            size_t len = len_base[sym] + bits(s, len_extra[sym]);
            int ds = decode(s, dist);
            if (s->err || ds < 0)
                return INFLATE_ETRUNC;
            if (ds >= 30)
                return INFLATE_EDATA;
            size_t d = dist_base[ds] + bits(s, dist_extra[ds]);
            if (s->err)
                return INFLATE_ETRUNC;
            if (d > s->out_pos)
                return INFLATE_EDATA;
            if (len > s->cap - s->out_pos)
                return INFLATE_EFULL;
            for (size_t i = 0; i < len; i++, s->out_pos++)
                s->out[s->out_pos] = s->out[s->out_pos - d]; /* may overlap */
        }
    }
}

static int fixed(struct state *s)
{
    static struct huff lit, dist;
    static bool ready;
    if (!ready) {
        uint8_t l[NLITLEN];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < NLITLEN; i++) l[i] = 8;
        build(&lit, l, NLITLEN);
        for (i = 0; i < NDIST; i++) l[i] = 5;
        build(&dist, l, NDIST);
        ready = true;
    }
    return codes(s, &lit, &dist);
}

static int dynamic(struct state *s)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
    if (s->err)
        return INFLATE_ETRUNC;
    if (nlen > 286 || ndist > 30)
        return INFLATE_EDATA;
    uint8_t lengths[NLITLEN + NDIST];
    memset(lengths, 0, sizeof lengths);
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)bits(s, 3);
    if (s->err)
        return INFLATE_ETRUNC;
    struct huff lencode, lit, dist;
    if (build(&lencode, lengths, 19) != 0)
        return INFLATE_EDATA; /* the code-length code must be complete */
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (s->err || sym < 0)
            return INFLATE_ETRUNC;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        uint8_t val = 0;
        int rep;
        if (sym == 16) {
            if (i == 0)
                return INFLATE_EDATA;
            val = lengths[i - 1];
            rep = 3 + (int)bits(s, 2);
        } else if (sym == 17) {
            rep = 3 + (int)bits(s, 3);
        } else {
            rep = 11 + (int)bits(s, 7);
        }
        if (s->err)
            return INFLATE_ETRUNC;
        if (i + rep > nlen + ndist)
            return INFLATE_EDATA;
        while (rep--)
            lengths[i++] = val;
    }
    if (lengths[256] == 0)
        return INFLATE_EDATA; /* no end-of-block code */
    int r = build(&lit, lengths, nlen);
    if (r < 0 || (r > 0 && nlen - lit.count[0] != 1))
        return INFLATE_EDATA;
    r = build(&dist, lengths + nlen, ndist);
    if (r < 0 || (r > 0 && ndist - dist.count[0] != 1))
        return INFLATE_EDATA;
    return codes(s, &lit, &dist);
}

static int stored(struct state *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0; /* to a byte boundary */
    if (s->in_len - s->in_pos < 4)
        return INFLATE_ETRUNC;
    unsigned len = s->in[s->in_pos] | (unsigned)s->in[s->in_pos + 1] << 8;
    unsigned nlen = s->in[s->in_pos + 2] | (unsigned)s->in[s->in_pos + 3] << 8;
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF))
        return INFLATE_EDATA;
    if (s->in_len - s->in_pos < len)
        return INFLATE_ETRUNC;
    if (s->cap - s->out_pos < len)
        return INFLATE_EFULL;
    memcpy(s->out + s->out_pos, s->in + s->in_pos, len);
    s->in_pos += len;
    s->out_pos += len;
    return INFLATE_OK;
}

uint32_t adler32(const uint8_t *p, size_t n)
{
    uint32_t a = 1, b = 0;
    while (n) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

int zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    if (in_len < 6 || (in[0] & 0x0F) != 8 || ((in[0] << 8) | in[1]) % 31 || (in[1] & 0x20))
        return INFLATE_EHEADER;
    struct state s = { in, in_len - 4, 2, 0, 0, out, cap, 0, false };
    int last, r;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (s.err)
            return INFLATE_ETRUNC;
        if (type == 0)
            r = stored(&s);
        else if (type == 1)
            r = fixed(&s);
        else if (type == 2)
            r = dynamic(&s);
        else
            r = INFLATE_EDATA;
        if (r)
            return r;
    } while (!last);
    const uint8_t *t = in + in_len - 4;
    uint32_t want = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
    if (adler32(out, s.out_pos) != want)
        return INFLATE_ECHECK;
    *out_len = s.out_pos;
    return INFLATE_OK;
}
