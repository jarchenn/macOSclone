#include "net/proto.h"
#include "lib.h"

uint32_t csum_add(uint32_t sum, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)p[0] << 8 | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint16_t inet_checksum(const void *data, size_t len) { return csum_fold(csum_add(0, data, len)); }

uint16_t l4_checksum(uint32_t src, uint32_t dst, uint8_t proto, const void *seg, size_t len)
{
    uint8_t pseudo[12];
    memcpy(pseudo, &src, 4);
    memcpy(pseudo + 4, &dst, 4);
    pseudo[8] = 0;
    pseudo[9] = proto;
    pseudo[10] = (uint8_t)(len >> 8);
    pseudo[11] = (uint8_t)len;
    uint32_t sum = csum_add(0, pseudo, sizeof pseudo);
    sum = csum_add(sum, seg, len);
    return csum_fold(sum);
}

bool ip_parse(const char *s, uint32_t *ip)
{
    uint32_t parts[4];
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return false;
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 3)
                return false;
            v = v * 10 + (uint32_t)(*s++ - '0');
        }
        if (v > 255)
            return false;
        parts[i] = v;
        if (i < 3 && *s++ != '.')
            return false;
    }
    if (*s)
        return false;
    *ip = IPV4(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

void ip_format(uint32_t ip, char *buf, size_t cap)
{
    k_snprintf(buf, cap, "%u.%u.%u.%u", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
}

/* ---- DNS ------------------------------------------------------------------ */

static bool label_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

int dns_build_query(uint8_t *buf, size_t cap, uint16_t id, const char *name)
{
    size_t nl = strlen(name);
    if (nl == 0 || nl > DNS_MAX_NAME || cap < 12 + nl + 2 + 4)
        return -1;
    memset(buf, 0, 12);
    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)id;
    buf[2] = 0x01; /* RD */
    buf[5] = 1;    /* QDCOUNT */
    size_t off = 12;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63)
            return -1;
        for (size_t i = 0; i < len; i++)
            if (!label_char(p[i]))
                return -1;
        buf[off++] = (uint8_t)len;
        memcpy(buf + off, p, len);
        off += len;
        p += len;
        if (*p == '.') {
            p++;
            if (!*p)
                break; /* trailing dot */
        }
    }
    buf[off++] = 0;
    buf[off++] = 0;
    buf[off++] = 1; /* QTYPE A */
    buf[off++] = 0;
    buf[off++] = 1; /* QCLASS IN */
    return (int)off;
}

/* Skips a (possibly compressed) name; returns the offset after it or 0. */
static size_t skip_name(const uint8_t *m, size_t len, size_t off)
{
    for (int guard = 0; guard < 128; guard++) {
        if (off >= len)
            return 0;
        uint8_t l = m[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xC0) == 0xC0)
            return off + 2 <= len ? off + 2 : 0; /* pointer ends the name */
        if (l & 0xC0)
            return 0;
        off += 1u + l;
    }
    return 0;
}

int dns_parse_a(const uint8_t *m, size_t len, uint16_t id, uint32_t *ip)
{
    if (len < 12)
        return DNS_E_MALFORMED;
    if (((uint16_t)m[0] << 8 | m[1]) != id)
        return DNS_E_MISMATCH;
    if (!(m[2] & 0x80))
        return DNS_E_MALFORMED; /* not a response */
    int rcode = m[3] & 0x0F;
    if (rcode == 3)
        return DNS_E_NXDOMAIN;
    if (rcode != 0)
        return DNS_E_SERVFAIL;
    unsigned qd = (unsigned)m[4] << 8 | m[5], an = (unsigned)m[6] << 8 | m[7];
    size_t off = 12;
    for (unsigned i = 0; i < qd; i++) {
        off = skip_name(m, len, off);
        if (!off || off + 4 > len)
            return DNS_E_MALFORMED;
        off += 4;
    }
    for (unsigned i = 0; i < an; i++) {
        off = skip_name(m, len, off);
        if (!off || off + 10 > len)
            return DNS_E_MALFORMED;
        uint16_t type = (uint16_t)(m[off] << 8 | m[off + 1]);
        uint16_t cls = (uint16_t)(m[off + 2] << 8 | m[off + 3]);
        uint16_t rdlen = (uint16_t)(m[off + 8] << 8 | m[off + 9]);
        off += 10;
        if (off + rdlen > len)
            return DNS_E_MALFORMED;
        if (type == 1 && cls == 1 && rdlen == 4) {
            memcpy(ip, m + off, 4);
            return DNS_OK;
        }
        off += rdlen; /* CNAME and others: keep looking */
    }
    return DNS_E_NOADDR;
}

/* ---- DHCP ----------------------------------------------------------------- */

size_t dhcp_build(uint8_t *buf, size_t cap, uint8_t type, uint32_t xid, const uint8_t mac[6],
                  uint32_t requested_ip, uint32_t server_id)
{
    if (cap < 300)
        return 0;
    memset(buf, 0, 300);
    buf[0] = 1;  /* BOOTREQUEST */
    buf[1] = 1;  /* Ethernet */
    buf[2] = 6;
    memcpy(buf + 4, &xid, 4);
    buf[10] = 0x80; /* broadcast flag: we cannot receive unicast before configuration */
    memcpy(buf + 28, mac, 6);
    buf[236] = 99; buf[237] = 130; buf[238] = 83; buf[239] = 99; /* magic cookie */
    size_t o = 240;
    buf[o++] = 53; buf[o++] = 1; buf[o++] = type;
    buf[o++] = 61; buf[o++] = 7; buf[o++] = 1;
    memcpy(buf + o, mac, 6);
    o += 6;
    if (requested_ip) {
        buf[o++] = 50; buf[o++] = 4;
        memcpy(buf + o, &requested_ip, 4);
        o += 4;
    }
    if (server_id) {
        buf[o++] = 54; buf[o++] = 4;
        memcpy(buf + o, &server_id, 4);
        o += 4;
    }
    buf[o++] = 55; buf[o++] = 4; buf[o++] = 1; buf[o++] = 3; buf[o++] = 6; buf[o++] = 51;
    buf[o++] = 12; buf[o++] = 6;
    memcpy(buf + o, "aurora", 6);
    o += 6;
    buf[o++] = 255;
    return o < 300 ? 300 : o;
}

int dhcp_parse(const uint8_t *p, size_t len, uint32_t xid, const uint8_t mac[6], struct dhcp_info *out)
{
    memset(out, 0, sizeof *out);
    if (len < 240 || p[0] != 2 || memcmp(p + 4, &xid, 4) || memcmp(p + 28, mac, 6))
        return -1;
    if (p[236] != 99 || p[237] != 130 || p[238] != 83 || p[239] != 99)
        return -1;
    memcpy(&out->yiaddr, p + 16, 4);
    size_t o = 240;
    while (o < len) {
        uint8_t code = p[o++];
        if (code == 0)
            continue;
        if (code == 255)
            break;
        if (o >= len)
            return -1;
        uint8_t l = p[o++];
        if (o + l > len)
            return -1;
        const uint8_t *v = p + o;
        switch (code) {
        case 53: if (l >= 1) out->type = v[0]; break;
        case 54: if (l >= 4) memcpy(&out->server_id, v, 4); break;
        case 1: if (l >= 4) memcpy(&out->netmask, v, 4); break;
        case 3: if (l >= 4) memcpy(&out->router, v, 4); break;
        case 6: if (l >= 4) memcpy(&out->dns, v, 4); break;
        case 51: if (l >= 4) out->lease_s = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3]; break;
        default: break;
        }
        o += l;
    }
    return out->type ? 0 : -1;
}
