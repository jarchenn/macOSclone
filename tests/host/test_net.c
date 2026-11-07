/* Network protocol helpers: checksums, IPv4 text, DNS, DHCP, TCP sequence
 * arithmetic. */
#include "harness.h"
#include "net/proto.h"
#include "lib.h"

const char *t_suite = "net";

TEST(checksums)
{
    /* RFC 1071-style example: IPv4 header from a real capture. */
    uint8_t h[20] = { 0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                      0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7 };
    uint16_t c = inet_checksum(h, 20);
    uint8_t *cb = (uint8_t *)&c;
    CHECK_EQ(cb[0], 0xb8); /* stored in network order */
    CHECK_EQ(cb[1], 0x61);
    memcpy(h + 10, &c, 2);
    CHECK_EQ(inet_checksum(h, 20), 0); /* verifying a correct header yields 0 */
    h[3] ^= 1;
    CHECK(inet_checksum(h, 20) != 0);
    /* odd length and UDP pseudo-header round trip */
    uint8_t seg[13] = { 0x04, 0x00, 0x00, 0x35, 0x00, 0x0d, 0, 0, 'h', 'e', 'l', 'l', 'o' };
    uint32_t src = IPV4(10, 0, 2, 15), dst = IPV4(10, 0, 2, 3);
    uint16_t u = l4_checksum(src, dst, 17, seg, sizeof seg);
    memcpy(seg + 6, &u, 2);
    CHECK_EQ(l4_checksum(src, dst, 17, seg, sizeof seg), 0);
    CHECK(l4_checksum(dst, src ^ 1, 17, seg, sizeof seg) != 0);
}

TEST(ip_text)
{
    uint32_t ip;
    char buf[16];
    CHECK(ip_parse("10.0.2.15", &ip));
    CHECK_EQ(ip, IPV4(10, 0, 2, 15));
    ip_format(ip, buf, sizeof buf);
    CHECK_STR(buf, "10.0.2.15");
    CHECK(ip_parse("255.255.255.255", &ip));
    CHECK_EQ(ip, 0xFFFFFFFFu);
    CHECK(!ip_parse("256.1.1.1", &ip));
    CHECK(!ip_parse("1.2.3", &ip));
    CHECK(!ip_parse("1.2.3.4.5", &ip));
    CHECK(!ip_parse("1..2.3", &ip));
    CHECK(!ip_parse("01234.1.1.1", &ip));
    CHECK(!ip_parse("example.com", &ip));
    CHECK(!ip_parse("", &ip));
}

TEST(dns_query_encoding)
{
    uint8_t q[300];
    int n = dns_build_query(q, sizeof q, 0xBEEF, "www.example.com");
    CHECK_EQ(n, 12 + 17 + 4);
    CHECK_EQ(q[0], 0xBE);
    CHECK_EQ(q[1], 0xEF);
    CHECK_EQ(q[2], 0x01);
    CHECK_EQ(q[5], 1);
    CHECK(memcmp(q + 12, "\x03www\x07" "example\x03" "com\x00", 17) == 0);
    CHECK_EQ(dns_build_query(q, sizeof q, 1, "trailing.dot."), 12 + 14 + 4);
    CHECK_EQ(dns_build_query(q, sizeof q, 1, ""), -1);
    CHECK_EQ(dns_build_query(q, sizeof q, 1, "a..b"), -1);
    CHECK_EQ(dns_build_query(q, sizeof q, 1, "bad name.com"), -1);
    char longlabel[80];
    memset(longlabel, 'x', 64);
    longlabel[64] = 0;
    CHECK_EQ(dns_build_query(q, sizeof q, 1, longlabel), -1);
    CHECK_EQ(dns_build_query(q, 20, 1, "example.com"), -1); /* buffer too small */
}

static size_t make_resp(uint8_t *m, uint16_t id, int rcode, bool cname_first)
{
    uint8_t q[64];
    int qn = dns_build_query(q, sizeof q, id, "aurora.test");
    memcpy(m, q, (size_t)qn);
    m[2] = 0x81;
    m[3] = (uint8_t)(0x80 | rcode);
    size_t o = (size_t)qn;
    int an = 0;
    if (cname_first) {
        uint8_t c[] = { 0xc0, 0x0c, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xc0, 0x0c };
        memcpy(m + o, c, sizeof c);
        o += sizeof c;
        an++;
    }
    uint8_t a[] = { 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 1, 2, 3 };
    memcpy(m + o, a, sizeof a);
    o += sizeof a;
    an++;
    m[7] = (uint8_t)(rcode ? 0 : an);
    return o;
}

TEST(dns_response_parsing)
{
    uint8_t m[512];
    uint32_t ip = 0;
    size_t n = make_resp(m, 0x1234, 0, false);
    CHECK_EQ(dns_parse_a(m, n, 0x1234, &ip), DNS_OK);
    CHECK_EQ(ip, IPV4(10, 1, 2, 3));
    n = make_resp(m, 0x1234, 0, true);
    ip = 0;
    CHECK_EQ(dns_parse_a(m, n, 0x1234, &ip), DNS_OK); /* CNAME skipped */
    CHECK_EQ(ip, IPV4(10, 1, 2, 3));
    CHECK_EQ(dns_parse_a(m, n, 0x9999, &ip), DNS_E_MISMATCH);
    n = make_resp(m, 7, 3, false);
    CHECK_EQ(dns_parse_a(m, n, 7, &ip), DNS_E_NXDOMAIN);
    n = make_resp(m, 7, 2, false);
    CHECK_EQ(dns_parse_a(m, n, 7, &ip), DNS_E_SERVFAIL);
    /* truncated at every length: never reads past the end */
    n = make_resp(m, 5, 0, true);
    for (size_t cut = 0; cut < n; cut++) {
        int r = dns_parse_a(m, cut, 5, &ip);
        CHECK(r != DNS_OK || cut == n);
    }
    /* compression pointer loop is rejected, not followed forever */
    n = make_resp(m, 6, 0, false);
    size_t ans = n - 16;
    m[ans] = 0xc0;
    m[ans + 1] = (uint8_t)ans; /* points at itself */
    CHECK(dns_parse_a(m, n, 6, &ip) != DNS_OK || ip == IPV4(10, 1, 2, 3));
    /* a query (QR=0) is not an answer */
    n = make_resp(m, 8, 0, false);
    m[2] = 0x01;
    CHECK_EQ(dns_parse_a(m, n, 8, &ip), DNS_E_MALFORMED);
}

TEST(dhcp_round_trip)
{
    uint8_t mac[6] = { 0x52, 0x54, 0, 0x12, 0x34, 0x56 };
    uint8_t pkt[400];
    size_t n = dhcp_build(pkt, sizeof pkt, DHCP_DISCOVER, 0x11223344u, mac, 0, 0);
    CHECK(n >= 300);
    CHECK_EQ(pkt[0], 1);
    CHECK(memcmp(pkt + 28, mac, 6) == 0);
    CHECK_EQ(pkt[240], 53);
    CHECK_EQ(pkt[242], DHCP_DISCOVER);
    CHECK_EQ(dhcp_build(pkt, 100, DHCP_DISCOVER, 1, mac, 0, 0), 0);
    /* Turn it into a server OFFER and parse it. */
    pkt[0] = 2;
    uint32_t yi = IPV4(10, 0, 2, 15);
    memcpy(pkt + 16, &yi, 4);
    size_t o = 240;
    uint8_t opts[] = { 53, 1, DHCP_OFFER, 54, 4, 10, 0, 2, 2, 1, 4, 255, 255, 255, 0, 3, 4, 10, 0, 2, 2,
                       6, 4, 10, 0, 2, 3, 51, 4, 0, 1, 0x51, 0x80, 0, 255 };
    memcpy(pkt + o, opts, sizeof opts);
    struct dhcp_info info;
    CHECK_EQ(dhcp_parse(pkt, o + sizeof opts, 0x11223344u, mac, &info), 0);
    CHECK_EQ(info.type, DHCP_OFFER);
    CHECK_EQ(info.yiaddr, yi);
    CHECK_EQ(info.router, IPV4(10, 0, 2, 2));
    CHECK_EQ(info.dns, IPV4(10, 0, 2, 3));
    CHECK_EQ(info.netmask, IPV4(255, 255, 255, 0));
    CHECK_EQ(info.server_id, IPV4(10, 0, 2, 2));
    CHECK_EQ(info.lease_s, 86400);
    CHECK_EQ(dhcp_parse(pkt, o + sizeof opts, 0x99, mac, &info), -1);     /* other transaction */
    uint8_t other[6] = { 1, 2, 3, 4, 5, 6 };
    CHECK_EQ(dhcp_parse(pkt, o + sizeof opts, 0x11223344u, other, &info), -1);
    pkt[o + 1] = 200; /* option length past the end */
    CHECK_EQ(dhcp_parse(pkt, o + sizeof opts, 0x11223344u, mac, &info), -1);
    pkt[236] = 0; /* bad cookie */
    CHECK_EQ(dhcp_parse(pkt, o + sizeof opts, 0x11223344u, mac, &info), -1);
}

TEST(sequence_arithmetic)
{
    CHECK(seq_lt(1, 2));
    CHECK(seq_lt(0xFFFFFFF0u, 0x10)); /* across the wrap */
    CHECK(seq_gt(0x10, 0xFFFFFFF0u));
    CHECK(seq_le(5, 5) && seq_ge(5, 5));
    CHECK(!seq_lt(5, 5));
    CHECK(seq_lt(0x7FFFFFFFu, 0x80000000u));
}

void run_tests(void)
{
    RUN(checksums);
    RUN(ip_text);
    RUN(dns_query_encoding);
    RUN(dns_response_parsing);
    RUN(dhcp_round_trip);
    RUN(sequence_arithmetic);
}
