/* TCP (RFC 793 subset): active open only, in-order receive, go-back-N
 * retransmission with exponential backoff, flow control by the advertised
 * window. No congestion control, no SACK, no window scaling, no listening
 * sockets. Runs in the network task; the blocking calls run in the caller's
 * task and wait on the TCB. */
#include "net/net_internal.h"
#include "core/sched.h"
#include "core/time.h"
#include "core/log.h"
#include "core/process.h"
#include "abi/aurora_abi.h"
#include "lib.h"

#define TCP_MAX 16
#define TCP_BUF 16384
#define TCP_MSS 1460
#define RTO_INITIAL 1000
#define RTO_MAX 8000
#define MAX_RETRIES 8
#define TIME_WAIT_MS 2000
#define FIN_WAIT2_MS 15000

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

enum { T_CLOSED, T_SYN_SENT, T_ESTABLISHED, T_FIN_WAIT_1, T_FIN_WAIT_2, T_CLOSE_WAIT, T_CLOSING, T_LAST_ACK, T_TIME_WAIT };
static const char *const state_names[] = { "CLOSED", "SYN_SENT", "ESTABLISHED", "FIN_WAIT_1", "FIN_WAIT_2",
                                           "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT" };

struct tcb {
    bool used, orphan;
    int state;
    uint32_t rip;
    uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd, rcv_nxt;
    uint16_t mss;
    uint8_t sbuf[TCP_BUF]; /* bytes from snd_una onwards (unacked + unsent) */
    uint32_t slen;
    uint8_t rbuf[TCP_BUF]; /* received, not yet read */
    uint32_t rhead, rlen;
    bool fin_pending, fin_sent, rx_fin;
    int err;
    uint64_t rto_at, deadline;
    uint32_t rto;
    int retries;
    uint64_t retransmits;
};

static struct tcb tcbs[TCP_MAX];
static uint16_t next_port = 50000;

const char *tcp_state_name(const struct tcb *t) { return state_names[t->state]; }

int tcp_active_count(void)
{
    int n = 0;
    for (int i = 0; i < TCP_MAX; i++)
        n += tcbs[i].used;
    return n;
}

void tcp_describe(const struct tcb *t, char *buf, size_t cap)
{
    char ip[16];
    ip_format(t->rip, ip, sizeof ip);
    k_snprintf(buf, cap, "tcp %5u -> %s:%u %s%s", t->lport, ip, t->rport, state_names[t->state], t->orphan ? " (closing)" : "");
}

struct tcb *tcp_open(void)
{
    for (int i = 0; i < TCP_MAX; i++) {
        if (!tcbs[i].used) {
            memset(&tcbs[i], 0, sizeof tcbs[i]);
            tcbs[i].used = true;
            tcbs[i].state = T_CLOSED;
            return &tcbs[i];
        }
    }
    return NULL;
}

static uint16_t rcv_window(const struct tcb *t)
{
    uint32_t w = TCP_BUF - t->rlen;
    return (uint16_t)MIN(w, 65535u);
}

static void xmit(struct tcb *t, uint32_t seq, uint8_t flags, const uint8_t *data, uint32_t len)
{
    uint8_t seg[24 + TCP_MSS];
    uint32_t hl = (flags & F_SYN) ? 24 : 20;
    memset(seg, 0, hl);
    seg[0] = (uint8_t)(t->lport >> 8);
    seg[1] = (uint8_t)t->lport;
    seg[2] = (uint8_t)(t->rport >> 8);
    seg[3] = (uint8_t)t->rport;
    uint32_t s = htonl(seq), a = htonl(t->rcv_nxt);
    memcpy(seg + 4, &s, 4);
    memcpy(seg + 8, &a, 4);
    seg[12] = (uint8_t)((hl / 4) << 4);
    seg[13] = flags;
    uint16_t w = rcv_window(t);
    seg[14] = (uint8_t)(w >> 8);
    seg[15] = (uint8_t)w;
    if (flags & F_SYN) {
        seg[20] = 2; /* MSS option */
        seg[21] = 4;
        seg[22] = (uint8_t)(TCP_MSS >> 8);
        seg[23] = (uint8_t)TCP_MSS;
    }
    if (len)
        memcpy(seg + hl, data, len);
    uint16_t c = l4_checksum(g_net.ip, t->rip, IP_PROTO_TCP, seg, hl + len);
    memcpy(seg + 16, &c, 2);
    ip_send(t->rip, IP_PROTO_TCP, seg, hl + len);
}

static void send_ack(struct tcb *t) { xmit(t, t->snd_nxt, F_ACK, NULL, 0); }

static void arm_rto(struct tcb *t)
{
    if (!t->rto)
        t->rto = RTO_INITIAL;
    t->rto_at = time_ms() + t->rto;
}

static void release_if_done(struct tcb *t)
{
    if (t->orphan && t->state == T_CLOSED)
        t->used = false;
}

static void become_closed(struct tcb *t, int err)
{
    t->state = T_CLOSED;
    if (err && !t->err)
        t->err = err;
    t->rto_at = 0;
    sched_wake_all(t);
    release_if_done(t);
}

/* Sends whatever data (and FIN) the peer's window allows. */
static void output(struct tcb *t)
{
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT && t->state != T_FIN_WAIT_1 && t->state != T_LAST_ACK &&
        t->state != T_CLOSING)
        return;
    uint32_t window = MIN(MAX(t->snd_wnd, 1u), 8u * t->mss); /* zero window: 1-byte probe */
    uint32_t off = t->snd_nxt - t->snd_una;
    if (t->fin_sent)
        return; /* everything, including FIN, is already in flight */
    bool sent = false;
    while (off < t->slen && off < window) {
        uint32_t n = MIN(MIN((uint32_t)t->mss, t->slen - off), window - off);
        xmit(t, t->snd_una + off, F_ACK | F_PSH, t->sbuf + off, n);
        off += n;
        t->snd_nxt = t->snd_una + off;
        sent = true;
    }
    if (t->fin_pending && off == t->slen) {
        xmit(t, t->snd_una + t->slen, F_FIN | F_ACK, NULL, 0);
        t->fin_sent = true;
        t->snd_nxt = t->snd_una + t->slen + 1;
        if (t->state == T_ESTABLISHED)
            t->state = T_FIN_WAIT_1;
        else if (t->state == T_CLOSE_WAIT)
            t->state = T_LAST_ACK;
        sent = true;
    }
    if (sent && !t->rto_at)
        arm_rto(t);
}

int tcp_connect(struct tcb *t, uint32_t ip, uint16_t port, uint64_t timeout_ms)
{
    if (t->state != T_CLOSED || t->err)
        return AE_INVAL;
    t->rip = ip;
    t->rport = port;
    t->lport = next_port++;
    if (next_port < 50000)
        next_port = 50000;
    t->mss = 536;
    t->iss = (uint32_t)(time_ms() * 64000u) ^ ((uint32_t)t->lport << 16);
    t->snd_una = t->iss;
    t->snd_nxt = t->iss + 1;
    t->state = T_SYN_SENT;
    t->rto = RTO_INITIAL;
    xmit(t, t->iss, F_SYN, NULL, 0);
    arm_rto(t);
    uint64_t deadline = time_ms() + timeout_ms;
    while (t->state == T_SYN_SENT) {
        uint64_t now = time_ms();
        if (now >= deadline) {
            become_closed(t, AE_TIMEDOUT);
            return AE_TIMEDOUT;
        }
        struct process *p = process_current();
        if (p && process_kill_pending(p))
            return AE_INTR;
        sched_wait_on(t, deadline - now);
    }
    if (t->state == T_ESTABLISHED)
        return 0;
    return t->err ? t->err : AE_CONNREFUSED;
}

long tcp_send(struct tcb *t, const void *buf, uint64_t len)
{
    const uint8_t *p = buf;
    uint64_t done = 0;
    while (done < len) {
        if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT)
            return done ? (long)done : (t->err ? t->err : AE_CONNRESET);
        if (t->fin_pending)
            return AE_INVAL;
        uint32_t space = TCP_BUF - t->slen;
        if (space == 0) {
            struct process *pr = process_current();
            if (pr && process_kill_pending(pr))
                return done ? (long)done : AE_INTR;
            if (!sched_wait_on(t, 30000))
                return done ? (long)done : AE_TIMEDOUT;
            continue;
        }
        uint32_t n = (uint32_t)MIN((uint64_t)space, len - done);
        memcpy(t->sbuf + t->slen, p + done, n);
        t->slen += n;
        done += n;
        output(t);
    }
    return (long)done;
}

long tcp_recv(struct tcb *t, void *buf, uint64_t len, uint64_t timeout_ms)
{
    uint64_t deadline = timeout_ms ? time_ms() + timeout_ms : UINT64_MAX;
    for (;;) {
        if (t->rlen) {
            uint8_t *dst = buf;
            uint32_t before = TCP_BUF - t->rlen;
            uint32_t n = (uint32_t)MIN((uint64_t)t->rlen, len);
            for (uint32_t i = 0; i < n; i++)
                dst[i] = t->rbuf[(t->rhead + i) % TCP_BUF];
            t->rhead = (t->rhead + n) % TCP_BUF;
            t->rlen -= n;
            if (before < t->mss && TCP_BUF - t->rlen >= t->mss && t->state != T_CLOSED)
                send_ack(t); /* window update */
            return (long)n;
        }
        if (t->rx_fin)
            return 0;
        if (t->state == T_CLOSED)
            return t->err ? t->err : 0;
        uint64_t now = time_ms();
        if (now >= deadline)
            return AE_TIMEDOUT;
        struct process *p = process_current();
        if (p && process_kill_pending(p))
            return AE_INTR;
        sched_wait_on(t, deadline == UINT64_MAX ? 0 : deadline - now);
    }
}

void tcp_close(struct tcb *t)
{
    t->orphan = true;
    switch (t->state) {
    case T_ESTABLISHED:
    case T_CLOSE_WAIT:
        t->fin_pending = true;
        t->deadline = time_ms() + FIN_WAIT2_MS + 30000;
        output(t);
        break;
    case T_SYN_SENT:
        become_closed(t, 0);
        break;
    default:
        release_if_done(t);
        break;
    }
}

static void send_reset(uint32_t src, uint16_t sport, uint16_t dport, uint32_t seq, uint32_t ack, bool has_ack)
{
    struct tcb tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.rip = src;
    tmp.lport = dport;
    tmp.rport = sport;
    tmp.rcv_nxt = ack;
    xmit(&tmp, has_ack ? seq : 0, (uint8_t)(F_RST | (has_ack ? 0 : F_ACK)), NULL, 0);
}

static void accept_data(struct tcb *t, const uint8_t *data, uint32_t len, bool fin)
{
    uint32_t space = TCP_BUF - t->rlen;
    uint32_t n = MIN(len, space);
    for (uint32_t i = 0; i < n; i++)
        t->rbuf[(t->rhead + t->rlen + i) % TCP_BUF] = data[i];
    t->rlen += n;
    t->rcv_nxt += n;
    if (fin && n == len) {
        t->rcv_nxt++;
        t->rx_fin = true;
        if (t->state == T_ESTABLISHED)
            t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT_1)
            t->state = T_CLOSING;
        else if (t->state == T_FIN_WAIT_2) {
            t->state = T_TIME_WAIT;
            t->deadline = time_ms() + TIME_WAIT_MS;
        }
    }
    if (n || fin)
        sched_wake_all(t);
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len)
{
    if (len < 20 || l4_checksum(src, dst, IP_PROTO_TCP, seg, len) != 0)
        return;
    uint16_t sport = (uint16_t)(seg[0] << 8 | seg[1]), dport = (uint16_t)(seg[2] << 8 | seg[3]);
    uint32_t seq, ack;
    memcpy(&seq, seg + 4, 4);
    memcpy(&ack, seg + 8, 4);
    seq = ntohl(seq);
    ack = ntohl(ack);
    uint32_t hl = (uint32_t)(seg[12] >> 4) * 4;
    uint8_t flags = seg[13];
    uint32_t window = (uint32_t)seg[14] << 8 | seg[15];
    if (hl < 20 || hl > len)
        return;
    const uint8_t *data = seg + hl;
    uint32_t dlen = (uint32_t)(len - hl);

    struct tcb *t = NULL;
    for (int i = 0; i < TCP_MAX; i++)
        if (tcbs[i].used && tcbs[i].state != T_CLOSED && tcbs[i].lport == dport && tcbs[i].rport == sport &&
            tcbs[i].rip == src)
            t = &tcbs[i];
    if (!t) {
        if (!(flags & F_RST)) /* no listener / connection: refuse */
            send_reset(src, sport, dport, ack, seq + dlen + ((flags & F_SYN) ? 1 : 0) + ((flags & F_FIN) ? 1 : 0),
                       (flags & F_ACK) != 0);
        return;
    }

    if (t->state == T_SYN_SENT) {
        if ((flags & F_ACK) && ack != t->iss + 1) {
            if (!(flags & F_RST))
                send_reset(src, sport, dport, ack, 0, true);
            return;
        }
        if (flags & F_RST) {
            if (flags & F_ACK)
                become_closed(t, AE_CONNREFUSED);
            return;
        }
        if ((flags & F_SYN) && (flags & F_ACK)) {
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->snd_wnd = window;
            t->retries = 0;
            t->rto = RTO_INITIAL;
            t->rto_at = 0;
            /* MSS option */
            for (uint32_t o = 20; o + 1 < hl;) {
                uint8_t kind = seg[o];
                if (kind == 0)
                    break;
                if (kind == 1) {
                    o++;
                    continue;
                }
                uint8_t ol = seg[o + 1];
                if (ol < 2 || o + ol > hl)
                    break;
                if (kind == 2 && ol == 4)
                    t->mss = (uint16_t)MIN((uint32_t)(seg[o + 2] << 8 | seg[o + 3]), (uint32_t)TCP_MSS);
                o += ol;
            }
            if (t->mss < 64)
                t->mss = 536;
            t->state = T_ESTABLISHED;
            send_ack(t);
            sched_wake_all(t);
        }
        return;
    }

    if (flags & F_RST) {
        if (seq_ge(seq, t->rcv_nxt) && seq_lt(seq, t->rcv_nxt + TCP_BUF))
            become_closed(t, AE_CONNRESET);
        return;
    }
    if (flags & F_SYN) { /* duplicate SYN-ACK: re-acknowledge */
        send_ack(t);
        return;
    }
    if (flags & F_ACK) {
        if (seq_gt(ack, t->snd_una) && seq_le(ack, t->snd_nxt)) {
            uint32_t acked = ack - t->snd_una;
            bool fin_acked = t->fin_sent && ack == t->snd_nxt;
            uint32_t data_acked = MIN(fin_acked ? acked - 1 : acked, t->slen);
            memmove(t->sbuf, t->sbuf + data_acked, t->slen - data_acked);
            t->slen -= data_acked;
            t->snd_una = ack;
            t->retries = 0;
            t->rto = RTO_INITIAL;
            t->rto_at = t->snd_una == t->snd_nxt ? 0 : time_ms() + t->rto;
            if (fin_acked) {
                if (t->state == T_FIN_WAIT_1)
                    t->state = T_FIN_WAIT_2;
                else if (t->state == T_CLOSING) {
                    t->state = T_TIME_WAIT;
                    t->deadline = time_ms() + TIME_WAIT_MS;
                } else if (t->state == T_LAST_ACK) {
                    become_closed(t, 0);
                    return;
                }
            }
            sched_wake_all(t);
        }
        t->snd_wnd = window;
    }
    if (dlen || (flags & F_FIN)) {
        if (seq == t->rcv_nxt && !t->rx_fin)
            accept_data(t, data, dlen, (flags & F_FIN) != 0);
        send_ack(t); /* in order: acknowledges; otherwise a duplicate ACK */
    }
    output(t);
}

void tcp_timers(uint64_t now)
{
    for (int i = 0; i < TCP_MAX; i++) {
        struct tcb *t = &tcbs[i];
        if (!t->used)
            continue;
        if (t->state == T_TIME_WAIT && now >= t->deadline) {
            become_closed(t, 0);
            continue;
        }
        if (t->orphan && t->deadline && now >= t->deadline && t->state != T_CLOSED) {
            become_closed(t, AE_TIMEDOUT); /* peer never finished closing */
            continue;
        }
        if (!t->rto_at || now < t->rto_at)
            continue;
        if (++t->retries > MAX_RETRIES) {
            klog("tcp: connection to port %u timed out after %d retransmissions", t->rport, MAX_RETRIES);
            become_closed(t, AE_TIMEDOUT);
            continue;
        }
        t->retransmits++;
        t->rto = MIN(t->rto * 2, (uint32_t)RTO_MAX);
        t->rto_at = now + t->rto;
        if (t->state == T_SYN_SENT) {
            xmit(t, t->iss, F_SYN, NULL, 0);
            continue;
        }
        /* Go back to the oldest unacknowledged byte and resend. */
        t->snd_nxt = t->snd_una;
        if (t->fin_sent) {
            t->fin_sent = false;
            if (t->state == T_FIN_WAIT_1)
                t->state = T_ESTABLISHED;
            else if (t->state == T_LAST_ACK)
                t->state = T_CLOSE_WAIT;
            else if (t->state == T_CLOSING)
                t->state = T_CLOSE_WAIT; /* peer's FIN already received */
        }
        uint64_t saved = t->rto_at;
        t->rto_at = 0;
        output(t);
        t->rto_at = saved;
    }
}
