/* Network stack core: Ethernet, ARP, IPv4, ICMP, UDP sockets, DHCP, DNS and
 * the network task. All packet processing happens in the network task;
 * blocking calls run in the caller's task and are woken by it. */
#include "net/net_internal.h"
#include "core/sched.h"
#include "core/time.h"
#include "core/log.h"
#include "drivers/pci.h"
#include "abi/aurora_abi.h"
#include "core/process.h"
#include "lib.h"

struct net_config g_net;
static struct task *net_task_ptr;
static uint8_t txbuf[ETH_FRAME_MAX];
static const uint8_t bcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

const struct net_config *net_info(void) { return &g_net; }
bool net_parse_ip(const char *s, uint32_t *ip) { return ip_parse(s, ip); }
void net_format_ip(uint32_t ip, char *buf, size_t cap) { ip_format(ip, buf, cap); }

const char *net_strerror(int err)
{
    switch (err) {
    case AE_TIMEDOUT: return "timed out";
    case AE_CONNREFUSED: return "connection refused";
    case AE_HOSTUNREACH: return "host unreachable";
    case AE_NETDOWN: return "network is down";
    case AE_NOTFOUND: return "name not found";
    case AE_CONNRESET: return "connection reset";
    case AE_ADDRINUSE: return "address in use";
    case AE_BADF: return "bad socket";
    case AE_INVAL: return "invalid argument";
    default: return "network error";
    }
}

/* ---- Ethernet / ARP --------------------------------------------------------- */

#define ARP_ENTRIES 16
static struct {
    uint32_t ip;
    uint8_t mac[6];
    uint64_t when;
    bool valid;
} arp[ARP_ENTRIES];
static int arp_wait_obj;

static void eth_send(const uint8_t dst[6], uint16_t type, const void *payload, size_t len)
{
    if (len + 14 > ETH_FRAME_MAX)
        return;
    memcpy(txbuf, dst, 6);
    memcpy(txbuf + 6, g_net.mac, 6);
    txbuf[12] = (uint8_t)(type >> 8);
    txbuf[13] = (uint8_t)type;
    memcpy(txbuf + 14, payload, len);
    if (vnet_send(txbuf, len + 14))
        g_net.tx_errors++;
    else
        g_net.tx_packets++;
}

static bool arp_lookup(uint32_t ip, uint8_t mac[6])
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp[i].valid && arp[i].ip == ip) {
            memcpy(mac, arp[i].mac, 6);
            return true;
        }
    return false;
}

static void arp_learn(uint32_t ip, const uint8_t mac[6])
{
    if (!ip || ip == IP_BROADCAST)
        return;
    int slot = 0;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp[i].valid && arp[i].ip == ip) {
            slot = i;
            break;
        }
        if (!arp[i].valid) {
            slot = i;
            oldest = 0;
        } else if (oldest && arp[i].when < oldest) {
            oldest = arp[i].when;
            slot = i;
        }
    }
    arp[slot].ip = ip;
    memcpy(arp[slot].mac, mac, 6);
    arp[slot].when = time_ms();
    arp[slot].valid = true;
    sched_wake_all(&arp_wait_obj);
}

static void arp_request(uint32_t ip)
{
    uint8_t p[28];
    p[0] = 0; p[1] = 1; p[2] = 0x08; p[3] = 0x00; p[4] = 6; p[5] = 4; p[6] = 0; p[7] = 1;
    memcpy(p + 8, g_net.mac, 6);
    memcpy(p + 14, &g_net.ip, 4);
    memset(p + 18, 0, 6);
    memcpy(p + 24, &ip, 4);
    eth_send(bcast_mac, ETH_TYPE_ARP, p, sizeof p);
}

static void arp_input(const uint8_t *p, size_t len)
{
    if (len < 28 || p[0] != 0 || p[1] != 1 || p[2] != 0x08 || p[3] != 0 || p[4] != 6 || p[5] != 4)
        return;
    uint16_t op = (uint16_t)(p[6] << 8 | p[7]);
    uint32_t spa, tpa;
    memcpy(&spa, p + 14, 4);
    memcpy(&tpa, p + 24, 4);
    if (g_net.configured && tpa == g_net.ip) {
        arp_learn(spa, p + 8);
        if (op == 1) {
            uint8_t r[28];
            memcpy(r, p, 6);
            r[6] = 0;
            r[7] = 2;
            memcpy(r + 8, g_net.mac, 6);
            memcpy(r + 14, &g_net.ip, 4);
            memcpy(r + 18, p + 8, 6);
            memcpy(r + 24, &spa, 4);
            eth_send(p + 8, ETH_TYPE_ARP, r, sizeof r);
        }
    } else if (op == 2) {
        arp_learn(spa, p + 8);
    }
}

static uint32_t next_hop(uint32_t dst)
{
    if (dst == IP_BROADCAST || !g_net.netmask)
        return dst;
    return ((dst & g_net.netmask) == (g_net.ip & g_net.netmask)) ? dst : g_net.gateway;
}

int arp_resolve(uint32_t dst, uint64_t timeout_ms)
{
    if (!g_net.up)
        return AE_NETDOWN;
    uint32_t hop = next_hop(dst);
    if (hop == IP_BROADCAST)
        return 0;
    uint8_t mac[6];
    uint64_t deadline = time_ms() + timeout_ms;
    while (!arp_lookup(hop, mac)) {
        if (time_ms() >= deadline)
            return AE_HOSTUNREACH;
        arp_request(hop);
        sched_wait_on(&arp_wait_obj, MIN(500ull, deadline - time_ms() + 1));
    }
    return 0;
}

/* ---- IPv4 ---------------------------------------------------------------- */

static uint16_t ip_id = 1;

int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    if (!g_net.up)
        return AE_NETDOWN;
    if (len + 20 > NET_MTU)
        return AE_INVAL;
    uint8_t mac[6];
    uint32_t hop = next_hop(dst);
    if (dst == IP_BROADCAST) {
        memcpy(mac, bcast_mac, 6);
    } else if (!arp_lookup(hop, mac)) {
        arp_request(hop);
        return AE_HOSTUNREACH;
    }
    static uint8_t pkt[NET_MTU];
    uint8_t *h = pkt;
    h[0] = 0x45;
    h[1] = 0;
    uint16_t total = (uint16_t)(20 + len);
    h[2] = (uint8_t)(total >> 8);
    h[3] = (uint8_t)total;
    h[4] = (uint8_t)(ip_id >> 8);
    h[5] = (uint8_t)ip_id;
    ip_id++;
    h[6] = 0x40; /* don't fragment */
    h[7] = 0;
    h[8] = 64;
    h[9] = proto;
    h[10] = h[11] = 0;
    memcpy(h + 12, &g_net.ip, 4);
    memcpy(h + 16, &dst, 4);
    uint16_t c = inet_checksum(h, 20);
    memcpy(h + 10, &c, 2);
    memcpy(pkt + 20, payload, len);
    eth_send(mac, ETH_TYPE_IP, pkt, total);
    return 0;
}

/* ---- ICMP --------------------------------------------------------------- */

#define PING_SLOTS 8
static struct {
    bool used, replied;
    uint16_t id, seq;
    uint32_t ip;
    uint64_t sent, rtt;
} pings[PING_SLOTS];
static uint16_t ping_id = 0x4155;

static void icmp_input(uint32_t src, const uint8_t *p, size_t len)
{
    if (len < 8 || inet_checksum(p, len) != 0)
        return;
    if (p[0] == 8 && p[1] == 0) { /* echo request: answer */
        static uint8_t r[NET_MTU];
        if (len > sizeof r)
            return;
        memcpy(r, p, len);
        r[0] = 0;
        r[2] = r[3] = 0;
        uint16_t c = inet_checksum(r, len);
        memcpy(r + 2, &c, 2);
        ip_send(src, IP_PROTO_ICMP, r, len);
    } else if (p[0] == 0 && p[1] == 0) { /* echo reply */
        uint16_t id = (uint16_t)(p[4] << 8 | p[5]), seq = (uint16_t)(p[6] << 8 | p[7]);
        for (int i = 0; i < PING_SLOTS; i++) {
            if (pings[i].used && !pings[i].replied && pings[i].id == id && pings[i].seq == seq && pings[i].ip == src) {
                pings[i].replied = true;
                pings[i].rtt = time_ms() - pings[i].sent;
                sched_wake_all(&pings[i]);
            }
        }
    }
}

int net_ping(uint32_t ip, uint16_t seq, uint64_t timeout_ms, uint64_t *rtt_ms)
{
    if (!g_net.configured)
        return AE_NETDOWN;
    int r = arp_resolve(ip, MIN(timeout_ms, 3000ull));
    if (r)
        return r;
    int slot = -1;
    for (int i = 0; i < PING_SLOTS; i++)
        if (!pings[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return AE_PROCLIM;
    pings[slot].used = true;
    pings[slot].replied = false;
    pings[slot].id = ping_id++;
    pings[slot].seq = seq;
    pings[slot].ip = ip;
    uint8_t m[64];
    memset(m, 0, sizeof m);
    m[0] = 8;
    m[4] = (uint8_t)(pings[slot].id >> 8);
    m[5] = (uint8_t)pings[slot].id;
    m[6] = (uint8_t)(seq >> 8);
    m[7] = (uint8_t)seq;
    memcpy(m + 8, "Aurora ping payload 0123456789abcdefghijklmnopqrstuvwxyz", 56);
    uint16_t c = inet_checksum(m, sizeof m);
    memcpy(m + 2, &c, 2);
    pings[slot].sent = time_ms();
    r = ip_send(ip, IP_PROTO_ICMP, m, sizeof m);
    uint64_t deadline = time_ms() + timeout_ms;
    while (r == 0 && !pings[slot].replied) {
        uint64_t now = time_ms();
        if (now >= deadline) {
            r = AE_TIMEDOUT;
            break;
        }
        sched_wait_on(&pings[slot], deadline - now);
    }
    if (r == 0 && rtt_ms)
        *rtt_ms = pings[slot].rtt;
    pings[slot].used = false;
    return r;
}

/* ---- UDP and the socket table ------------------------------------------- */

#define MAX_SOCKETS 16
#define DGRAM_QUEUE 6
#define DGRAM_MAX 1472

struct datagram {
    uint32_t ip;
    uint16_t port;
    uint16_t len;
    uint8_t data[DGRAM_MAX];
};

struct socket {
    bool used;
    uint8_t type;
    uint16_t lport;
    struct tcb *tcb;
    struct datagram q[DGRAM_QUEUE];
    int qh, qn;
    uint32_t dropped;
};

static struct socket socks[MAX_SOCKETS];
static uint16_t next_ephemeral = 49152;

static struct socket *sock_get(int s)
{
    return (s >= 0 && s < MAX_SOCKETS && socks[s].used) ? &socks[s] : NULL;
}

int sock_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SOCKETS; i++)
        n += socks[i].used;
    return n;
}

static bool udp_port_used(uint16_t port)
{
    for (int i = 0; i < MAX_SOCKETS; i++)
        if (socks[i].used && socks[i].type == AURORA_SOCK_UDP && socks[i].lport == port)
            return true;
    return false;
}

int sock_open(uint32_t type)
{
    if (!g_net.up)
        return AE_NETDOWN;
    if (type != AURORA_SOCK_TCP && type != AURORA_SOCK_UDP)
        return AE_INVAL;
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].used = true;
            socks[i].type = (uint8_t)type;
            if (type == AURORA_SOCK_TCP) {
                socks[i].tcb = tcp_open();
                if (!socks[i].tcb) {
                    socks[i].used = false;
                    return AE_PROCLIM;
                }
            }
            return i;
        }
    }
    return AE_PROCLIM;
}

int sock_bind(int s, uint16_t port)
{
    struct socket *k = sock_get(s);
    if (!k || k->type != AURORA_SOCK_UDP)
        return AE_BADF;
    if (port == 0 || udp_port_used(port))
        return AE_ADDRINUSE;
    k->lport = port;
    return 0;
}

static void udp_autobind(struct socket *k)
{
    if (k->lport)
        return;
    for (int tries = 0; tries < 16384; tries++) {
        uint16_t p = next_ephemeral++;
        if (next_ephemeral < 49152)
            next_ephemeral = 49152;
        if (!udp_port_used(p)) {
            k->lport = p;
            return;
        }
    }
}

static int udp_send_raw(uint16_t sport, uint32_t ip, uint16_t dport, const void *buf, size_t len)
{
    if (len > DGRAM_MAX)
        return AE_INVAL;
    static uint8_t seg[8 + DGRAM_MAX];
    seg[0] = (uint8_t)(sport >> 8);
    seg[1] = (uint8_t)sport;
    seg[2] = (uint8_t)(dport >> 8);
    seg[3] = (uint8_t)dport;
    uint16_t ul = (uint16_t)(8 + len);
    seg[4] = (uint8_t)(ul >> 8);
    seg[5] = (uint8_t)ul;
    seg[6] = seg[7] = 0;
    memcpy(seg + 8, buf, len);
    uint16_t c = l4_checksum(g_net.ip, ip, IP_PROTO_UDP, seg, ul);
    if (c == 0)
        c = 0xFFFF;
    memcpy(seg + 6, &c, 2);
    return ip_send(ip, IP_PROTO_UDP, seg, ul);
}

long sock_sendto(int s, const void *buf, uint64_t len, uint32_t ip, uint16_t port)
{
    struct socket *k = sock_get(s);
    if (!k || k->type != AURORA_SOCK_UDP)
        return AE_BADF;
    if (len > DGRAM_MAX || port == 0)
        return AE_INVAL;
    if (!g_net.configured && ip != IP_BROADCAST)
        return AE_NETDOWN;
    udp_autobind(k);
    int r = arp_resolve(ip, 3000);
    if (r)
        return r;
    r = udp_send_raw(k->lport, ip, port, buf, (size_t)len);
    return r ? r : (long)len;
}

long sock_recvfrom(int s, void *buf, uint64_t len, uint32_t *ip, uint16_t *port, uint64_t timeout_ms)
{
    struct socket *k = sock_get(s);
    if (!k || k->type != AURORA_SOCK_UDP)
        return AE_BADF;
    udp_autobind(k);
    uint64_t deadline = time_ms() + (timeout_ms ? timeout_ms : 1ull << 40);
    while (k->qn == 0) {
        uint64_t now = time_ms();
        if (now >= deadline)
            return AE_TIMEDOUT;
        struct task *t = sched_current();
        if (t->proc && process_kill_pending(t->proc))
            return AE_INTR;
        sched_wait_on(k, deadline - now);
        if (!sock_get(s))
            return AE_BADF;
    }
    struct datagram *d = &k->q[k->qh];
    uint64_t n = MIN(len, (uint64_t)d->len);
    memcpy(buf, d->data, n);
    if (ip)
        *ip = d->ip;
    if (port)
        *port = d->port;
    k->qh = (k->qh + 1) % DGRAM_QUEUE;
    k->qn--;
    return (long)n;
}

static void udp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t len)
{
    if (len < 8)
        return;
    uint16_t sport = (uint16_t)(p[0] << 8 | p[1]), dport = (uint16_t)(p[2] << 8 | p[3]);
    uint16_t ul = (uint16_t)(p[4] << 8 | p[5]);
    if (ul < 8 || ul > len)
        return;
    uint16_t c;
    memcpy(&c, p + 6, 2);
    if (c && l4_checksum(src, dst, IP_PROTO_UDP, p, ul) != 0) {
        g_net.rx_dropped++;
        return;
    }
    for (int i = 0; i < MAX_SOCKETS; i++) {
        struct socket *k = &socks[i];
        if (!k->used || k->type != AURORA_SOCK_UDP || k->lport != dport)
            continue;
        if (k->qn >= DGRAM_QUEUE) {
            k->dropped++; /* queue full: drop the newest datagram */
            return;
        }
        struct datagram *d = &k->q[(k->qh + k->qn) % DGRAM_QUEUE];
        d->ip = src;
        d->port = sport;
        d->len = (uint16_t)MIN((size_t)(ul - 8), (size_t)DGRAM_MAX);
        memcpy(d->data, p + 8, d->len);
        k->qn++;
        sched_wake_all(k);
        return;
    }
}

int sock_connect(int s, uint32_t ip, uint16_t port, uint64_t timeout_ms)
{
    struct socket *k = sock_get(s);
    if (!k || k->type != AURORA_SOCK_TCP)
        return AE_BADF;
    if (!g_net.configured)
        return AE_NETDOWN;
    int r = arp_resolve(ip, MIN(timeout_ms, 3000ull));
    if (r)
        return r;
    return tcp_connect(k->tcb, ip, port, timeout_ms);
}

long sock_send(int s, const void *buf, uint64_t len)
{
    struct socket *k = sock_get(s);
    if (!k || k->type != AURORA_SOCK_TCP)
        return AE_BADF;
    return tcp_send(k->tcb, buf, len);
}

long sock_recv(int s, void *buf, uint64_t len, uint64_t timeout_ms)
{
    struct socket *k = sock_get(s);
    if (!k)
        return AE_BADF;
    if (k->type == AURORA_SOCK_UDP)
        return sock_recvfrom(s, buf, len, NULL, NULL, timeout_ms);
    return tcp_recv(k->tcb, buf, len, timeout_ms);
}

int sock_poll(int s)
{
    struct socket *k = sock_get(s);
    if (!k)
        return 4;
    if (k->type == AURORA_SOCK_UDP)
        return (k->qn ? 1 : 0) | 2;
    return k->tcb ? tcp_poll(k->tcb) : 4;
}

int sock_close(int s)
{
    struct socket *k = sock_get(s);
    if (!k)
        return AE_BADF;
    if (k->tcb)
        tcp_close(k->tcb); /* the TCB lingers until the FIN handshake ends */
    k->used = false;
    sched_wake_all(k);
    return 0;
}

/* ---- IP receive ------------------------------------------------------------ */

static void ip_input(const uint8_t *eth_src, const uint8_t *p, size_t len)
{
    if (len < 20 || (p[0] >> 4) != 4)
        return;
    size_t ihl = (size_t)(p[0] & 0x0F) * 4;
    uint16_t total = (uint16_t)(p[2] << 8 | p[3]);
    if (ihl < 20 || total < ihl || total > len || inet_checksum(p, ihl) != 0) {
        g_net.rx_dropped++;
        return;
    }
    uint16_t frag = (uint16_t)(p[6] << 8 | p[7]);
    if ((frag & 0x2000) || (frag & 0x1FFF)) {
        g_net.rx_dropped++; /* fragments are not reassembled */
        return;
    }
    uint32_t src, dst;
    memcpy(&src, p + 12, 4);
    memcpy(&dst, p + 16, 4);
    bool for_us = dst == IP_BROADCAST || (g_net.configured && dst == g_net.ip) || !g_net.configured;
    if (!for_us)
        return;
    if (g_net.configured && g_net.netmask && (src & g_net.netmask) == (g_net.ip & g_net.netmask))
        arp_learn(src, eth_src);
    const uint8_t *payload = p + ihl;
    size_t plen = total - ihl;
    switch (p[9]) {
    case IP_PROTO_ICMP: icmp_input(src, payload, plen); break;
    case IP_PROTO_UDP: udp_input(src, dst, payload, plen); break;
    case IP_PROTO_TCP: tcp_input(src, dst, payload, plen); break;
    default: break;
    }
}

static void net_rx(const uint8_t *f, size_t len)
{
    if (len < 14)
        return;
    g_net.rx_packets++;
    if (memcmp(f, g_net.mac, 6) && memcmp(f, bcast_mac, 6))
        return;
    uint16_t type = (uint16_t)(f[12] << 8 | f[13]);
    if (type == ETH_TYPE_ARP)
        arp_input(f + 14, len - 14);
    else if (type == ETH_TYPE_IP)
        ip_input(f + 6, f + 14, len - 14);
}

/* ---- DHCP (runs inside the network task) -------------------------------- */

enum { DH_IDLE, DH_DISCOVER, DH_REQUEST, DH_BOUND, DH_FAILED };
static int dh_state = DH_IDLE;
static int dh_sock = -1;
static uint32_t dh_xid, dh_offer_ip, dh_server;
static uint64_t dh_next;
static int dh_tries;
static int dh_waiters;

static void dhcp_send(uint8_t type)
{
    static uint8_t m[320];
    size_t n = dhcp_build(m, sizeof m, type, dh_xid, g_net.mac, type == DHCP_REQUEST ? dh_offer_ip : 0,
                          type == DHCP_REQUEST ? dh_server : 0);
    udp_send_raw(68, IP_BROADCAST, 67, m, n);
}

static void dhcp_start(void)
{
    if (dh_sock < 0) {
        dh_sock = sock_open(AURORA_SOCK_UDP);
        if (dh_sock >= 0)
            socks[dh_sock].lport = 68;
    }
    dh_xid = (uint32_t)(time_ms() * 2654435761u) ^ 0xA0A0A0A0u;
    dh_state = DH_DISCOVER;
    dh_tries = 0;
    dh_next = 0;
}

static void dhcp_tick(uint64_t now)
{
    if (dh_sock < 0 || (dh_state != DH_DISCOVER && dh_state != DH_REQUEST))
        return;
    struct socket *k = &socks[dh_sock];
    while (k->qn) {
        struct datagram *d = &k->q[k->qh];
        struct dhcp_info info;
        bool ok = dhcp_parse(d->data, d->len, dh_xid, g_net.mac, &info) == 0;
        k->qh = (k->qh + 1) % DGRAM_QUEUE;
        k->qn--;
        if (!ok)
            continue;
        if (dh_state == DH_DISCOVER && info.type == DHCP_OFFER) {
            dh_offer_ip = info.yiaddr;
            dh_server = info.server_id;
            dh_state = DH_REQUEST;
            dh_tries = 0;
            dhcp_send(DHCP_REQUEST);
            dh_next = now + 2000;
        } else if (dh_state == DH_REQUEST && info.type == DHCP_ACK) {
            g_net.ip = info.yiaddr;
            g_net.netmask = info.netmask ? info.netmask : IPV4(255, 255, 255, 0);
            g_net.gateway = info.router;
            g_net.dns = info.dns;
            g_net.configured = true;
            dh_state = DH_BOUND;
            char a[16], gw[16], dns[16];
            ip_format(g_net.ip, a, sizeof a);
            ip_format(g_net.gateway, gw, sizeof gw);
            ip_format(g_net.dns, dns, sizeof dns);
            klog("net: DHCP lease %s gateway %s dns %s (lease %us)", a, gw, dns, info.lease_s);
            TEST_EVENT("NET_UP ip=%s gw=%s dns=%s", a, gw, dns);
            sched_wake_all(&dh_waiters);
            return;
        } else if (dh_state == DH_REQUEST && info.type == DHCP_NAK) {
            dh_state = DH_DISCOVER;
            dh_next = 0;
        }
    }
    if (now >= dh_next) {
        if (++dh_tries > 6) {
            dh_state = DH_FAILED;
            klog("net: DHCP failed (no server answered)");
            sched_wake_all(&dh_waiters);
            return;
        }
        dhcp_send(dh_state == DH_DISCOVER ? DHCP_DISCOVER : DHCP_REQUEST);
        dh_next = now + 1000ull * (uint64_t)dh_tries;
    }
}

int net_dhcp(uint64_t timeout_ms)
{
    if (!g_net.up)
        return AE_NETDOWN;
    if (dh_state != DH_DISCOVER && dh_state != DH_REQUEST) {
        g_net.configured = false;
        dhcp_start();
        sched_wake(net_task_ptr);
    }
    uint64_t deadline = time_ms() + timeout_ms;
    while (dh_state == DH_DISCOVER || dh_state == DH_REQUEST) {
        uint64_t now = time_ms();
        if (now >= deadline)
            return AE_TIMEDOUT;
        sched_wait_on(&dh_waiters, deadline - now);
    }
    return g_net.configured ? 0 : AE_TIMEDOUT;
}

/* ---- DNS ---------------------------------------------------------------- */

int net_resolve_via(const char *name, uint32_t server, uint16_t port, uint32_t *ip, uint64_t timeout_ms)
{
    if (ip_parse(name, ip))
        return 0;
    if (!strcmp(name, "localhost")) {
        *ip = IPV4(127, 0, 0, 1);
        return 0;
    }
    if (!g_net.configured)
        return AE_NETDOWN;
    if (!server)
        return AE_NOTFOUND;
    int s = sock_open(AURORA_SOCK_UDP);
    if (s < 0)
        return s;
    static uint16_t qid = 0x5A5A;
    uint8_t q[300], resp[DGRAM_MAX];
    int r = AE_TIMEDOUT;
    uint64_t per_try = MAX(timeout_ms / 3, 500ull);
    for (int attempt = 0; attempt < 3 && r == AE_TIMEDOUT; attempt++) {
        uint16_t id = qid++;
        int qlen = dns_build_query(q, sizeof q, id, name);
        if (qlen < 0) {
            r = AE_INVAL;
            break;
        }
        long sr = sock_sendto(s, q, (uint64_t)qlen, server, port);
        if (sr < 0) {
            r = (int)sr;
            break;
        }
        uint64_t deadline = time_ms() + per_try;
        for (;;) {
            uint64_t now = time_ms();
            if (now >= deadline)
                break;
            uint32_t from;
            uint16_t fport;
            long n = sock_recvfrom(s, resp, sizeof resp, &from, &fport, deadline - now);
            if (n < 0)
                break;
            if (from != server || fport != port)
                continue;
            int d = dns_parse_a(resp, (size_t)n, id, ip);
            if (d == DNS_E_MISMATCH)
                continue; /* a late answer to an earlier attempt */
            r = d == DNS_OK ? 0 : AE_NOTFOUND;
            break;
        }
    }
    sock_close(s);
    return r;
}

int net_resolve(const char *name, uint32_t *ip, uint64_t timeout_ms)
{
    return net_resolve_via(name, g_net.dns, 53, ip, timeout_ms);
}

/* ---- the network task --------------------------------------------------- */

static void net_main(void *arg)
{
    UNUSED(arg);
    dhcp_start();
    for (;;) {
        vnet_poll(net_rx);
        uint64_t now = time_ms();
        dhcp_tick(now);
        tcp_timers(now);
        sched_sleep_ms(4); /* poll at roughly the timer rate */
    }
}

void net_init(void)
{
    if (!vnet_init(g_net.mac)) {
        klog("net: no network card");
        return;
    }
    str_copy(g_net.driver, "virtio-net", sizeof g_net.driver);
    g_net.up = true;
    klog("net: virtio-net, MAC %02x:%02x:%02x:%02x:%02x:%02x", g_net.mac[0], g_net.mac[1], g_net.mac[2],
         g_net.mac[3], g_net.mac[4], g_net.mac[5]);
    net_task_ptr = task_create("net", net_main, NULL, NULL);
    if (!net_task_ptr)
        klog("net: cannot start the network task");
}
