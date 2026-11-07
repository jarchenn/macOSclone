/* Minimal, bounded printf-style formatter (no floating point, no heap).
 *
 * Supported: %% %c %s %d %i %u %x %X %p, length modifiers l, ll, z,
 * flags '-' and '0', field width (number or *), precision for %s (.N or .*).
 * Output is always NUL-terminated when cap > 0. Returns the number of
 * characters that would have been written (like C99 snprintf).
 */
#include "lib.h"

struct out {
    char *buf;
    size_t cap;
    size_t len;
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->cap)
        o->buf[o->len] = c;
    o->len++;
}

static void put_padded(struct out *o, const char *s, size_t n, int width, bool left, char padc)
{
    int pad = width > (int)n ? width - (int)n : 0;
    if (!left)
        while (pad-- > 0)
            put(o, padc);
    for (size_t i = 0; i < n; i++)
        put(o, s[i]);
    if (left)
        while (pad-- > 0)
            put(o, ' ');
}

static size_t fmt_utoa(uint64_t v, unsigned base, bool upper, char *tmp)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char rev[24];
    size_t n = 0;
    do {
        rev[n++] = digits[v % base];
        v /= base;
    } while (v);
    for (size_t i = 0; i < n; i++)
        tmp[i] = rev[n - 1 - i];
    return n;
}

int k_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
    struct out o = { buf, cap, 0 };
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        p++;
        bool left = false, zero = false;
        for (;; p++) {
            if (*p == '-')
                left = true;
            else if (*p == '0')
                zero = true;
            else
                break;
        }
        int width = 0;
        if (*p == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = true;
                width = -width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9')
                width = width * 10 + (*p++ - '0');
        }
        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            if (*p == '*') {
                prec = va_arg(ap, int);
                p++;
            } else {
                while (*p >= '0' && *p <= '9')
                    prec = prec * 10 + (*p++ - '0');
            }
        }
        int lng = 0;
        while (*p == 'l') {
            lng++;
            p++;
        }
        if (*p == 'z') {
            lng = 2;
            p++;
        }
        char tmp[24];
        char padc = (zero && !left) ? '0' : ' ';
        switch (*p) {
        case '%':
            put(&o, '%');
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            put_padded(&o, &c, 1, width, left, ' ');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            size_t n = 0;
            while (s[n] && (prec < 0 || n < (size_t)prec))
                n++;
            put_padded(&o, s, n, width, left, ' ');
            break;
        }
        case 'd':
        case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            bool neg = v < 0;
            uint64_t u = neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            char num[25];
            size_t n = 0;
            if (neg)
                num[n++] = '-';
            n += fmt_utoa(u, 10, false, num + n);
            if (neg && padc == '0') {
                put(&o, '-');
                put_padded(&o, num + 1, n - 1, width - 1, left, padc);
            } else {
                put_padded(&o, num, n, width, left, padc);
            }
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v = lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned);
            unsigned base = *p == 'u' ? 10 : 16;
            size_t n = fmt_utoa(v, base, *p == 'X', tmp);
            put_padded(&o, tmp, n, width, left, padc);
            break;
        }
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            put(&o, '0');
            put(&o, 'x');
            size_t n = fmt_utoa(v, 16, false, tmp);
            put_padded(&o, tmp, n, 16, false, '0');
            break;
        }
        case '\0':
            p--; /* trailing '%': stop */
            break;
        default:
            put(&o, '%');
            put(&o, *p);
            break;
        }
    }
    if (cap)
        buf[o.len < cap ? o.len : cap - 1] = '\0';
    return (int)o.len;
}

int k_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = k_vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return r;
}
