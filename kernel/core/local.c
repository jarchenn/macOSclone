/* Local message sockets with descriptor passing (core/local.h). */
#include "core/local.h"
#include "core/file.h"
#include "core/process.h"
#include "core/sched.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "kalloc.h"
#include "lib.h"

struct local_msg {
    struct local_msg *next;
    uint32_t len;
    uint32_t nfds;
    struct file *fds[AURORA_MSG_FDS];
    uint8_t data[];
};

struct local_end {
    struct local_end *peer;   /* NULL once the peer has closed */
    struct local_msg *head, *tail;
    uint32_t qbytes, qmsgs;
    bool peer_was_connected;
};

struct local_listener {
    char name[AURORA_LOCAL_NAME_MAX + 1];
    int backlog;
    int npending;
    struct file *pending[LOCAL_BACKLOG_MAX]; /* server ends waiting for accept */
    bool closed;
};

static struct local_listener *names[LOCAL_NAMES_MAX];

static void wake(const void *obj)
{
    sched_wake_all(obj);
    file_poll_wake();
}

static void msg_free(struct local_msg *m)
{
    for (uint32_t i = 0; i < m->nfds; i++)
        file_put(m->fds[i]);
    kfree(m);
}

static int make_pair(struct file **a, struct file **b)
{
    struct local_end *ea = kzalloc(sizeof *ea), *eb = kzalloc(sizeof *eb);
    struct file *fa = file_new(FK_LOCAL, AURORA_O_RDWR), *fb = file_new(FK_LOCAL, AURORA_O_RDWR);
    if (!ea || !eb || !fa || !fb) {
        if (ea) kfree(ea);
        if (eb) kfree(eb);
        if (fa) kfree(fa);
        if (fb) kfree(fb);
        return AE_NOMEM;
    }
    ea->peer = eb;
    eb->peer = ea;
    ea->peer_was_connected = eb->peer_was_connected = true;
    fa->lend = ea;
    fb->lend = eb;
    *a = fa;
    *b = fb;
    return 0;
}

int local_pair(struct file **a, struct file **b) { return make_pair(a, b); }

void local_end_close(struct local_end *e)
{
    if (e->peer) {
        e->peer->peer = NULL;
        wake(e->peer);
    }
    struct local_msg *m = e->head;
    while (m) {
        struct local_msg *n = m->next;
        msg_free(m);
        m = n;
    }
    wake(e);
    kfree(e);
}

/* ---- names ----------------------------------------------------------------- */

static struct local_listener *find_name(const char *name)
{
    for (int i = 0; i < LOCAL_NAMES_MAX; i++)
        if (names[i] && !strcmp(names[i]->name, name))
            return names[i];
    return NULL;
}

static bool name_ok(const char *name)
{
    size_t n = strlen(name);
    return n > 0 && n <= AURORA_LOCAL_NAME_MAX;
}

int local_listen(const char *name, int backlog, struct file **out)
{
    if (!name_ok(name))
        return AE_INVAL;
    if (find_name(name))
        return AE_ADDRINUSE;
    int slot = -1;
    for (int i = 0; i < LOCAL_NAMES_MAX && slot < 0; i++)
        if (!names[i])
            slot = i;
    if (slot < 0)
        return AE_MFILE;
    struct local_listener *l = kzalloc(sizeof *l);
    struct file *f = file_new(FK_LISTEN, AURORA_O_RDWR);
    if (!l || !f) {
        if (l) kfree(l);
        if (f) kfree(f);
        return AE_NOMEM;
    }
    str_copy(l->name, name, sizeof l->name);
    l->backlog = backlog <= 0 ? 4 : MIN(backlog, LOCAL_BACKLOG_MAX);
    names[slot] = l;
    f->llisten = l;
    *out = f;
    return 0;
}

void local_listener_close(struct local_listener *l)
{
    for (int i = 0; i < LOCAL_NAMES_MAX; i++)
        if (names[i] == l)
            names[i] = NULL;
    for (int i = 0; i < l->npending; i++)
        file_put(l->pending[i]); /* the client sees its peer close */
    l->npending = 0;
    l->closed = true;
    wake(l);
    kfree(l);
}

int local_connect(const char *name, struct file **out)
{
    if (!name_ok(name))
        return AE_INVAL;
    struct local_listener *l = find_name(name);
    if (!l)
        return AE_CONNREFUSED;
    if (l->npending >= l->backlog)
        return AE_AGAIN;
    struct file *c, *s;
    int r = make_pair(&c, &s);
    if (r)
        return r;
    l->pending[l->npending++] = s;
    wake(l);
    *out = c;
    return 0;
}

int local_accept(struct file *lf, bool nonblock, struct file **out)
{
    if (lf->kind != FK_LISTEN)
        return AE_INVAL;
    for (;;) {
        struct local_listener *l = lf->llisten;
        if (l->npending) {
            *out = l->pending[0];
            for (int i = 1; i < l->npending; i++)
                l->pending[i - 1] = l->pending[i];
            l->npending--;
            return 0;
        }
        if (nonblock || (lf->flags & AURORA_O_NONBLOCK))
            return AE_AGAIN;
        if (task_interrupted(sched_current()))
            return AE_INTR;
        uint64_t fl = irq_save();
        if (!l->npending)
            sched_wait_on(l, 0);
        irq_restore(fl);
    }
}

/* ---- data ------------------------------------------------------------------ */

long local_send(struct process *p, struct file *f, uint64_t ubuf, uint64_t len, const int32_t *fds, uint32_t nfds,
                bool nonblock)
{
    if (f->kind != FK_LOCAL)
        return AE_NOTSOCK;
    if (len > AURORA_MSG_MAX || nfds > AURORA_MSG_FDS)
        return AE_INVAL;
    struct file *pass[AURORA_MSG_FDS];
    for (uint32_t i = 0; i < nfds; i++) {
        pass[i] = proc_fd_get(p, fds[i]);
        if (!pass[i])
            return AE_BADF;
        if (pass[i]->kind == FK_LOCAL || pass[i]->kind == FK_LISTEN)
            return AE_INVAL; /* no socket-in-socket cycles */
    }
    struct local_msg *m = kmalloc(sizeof *m + (len ? len : 1));
    if (!m)
        return AE_NOMEM;
    if (len && copy_from_user(p, m->data, ubuf, len)) {
        kfree(m);
        return AE_FAULT;
    }
    m->next = NULL;
    m->len = (uint32_t)len;
    m->nfds = 0;
    for (;;) {
        struct local_end *e = f->lend;
        struct local_end *peer = e->peer;
        if (!peer) {
            kfree(m);
            return AE_PIPE;
        }
        bool room = peer->qmsgs < LOCAL_QUEUE_MSGS &&
                    (peer->qbytes + len <= LOCAL_QUEUE_BYTES || peer->qmsgs == 0);
        if (room) {
            /* Take references only now: the descriptors might have been
             * closed while we slept, which proc_fd_get would have shown. */
            for (uint32_t i = 0; i < nfds; i++) {
                struct file *pf = proc_fd_get(p, fds[i]);
                if (!pf || pf != pass[i]) {
                    for (uint32_t k = 0; k < m->nfds; k++)
                        file_put(m->fds[k]);
                    kfree(m);
                    return AE_BADF;
                }
                file_ref(pf);
                m->fds[m->nfds++] = pf;
            }
            if (peer->tail)
                peer->tail->next = m;
            else
                peer->head = m;
            peer->tail = m;
            peer->qbytes += (uint32_t)len;
            peer->qmsgs++;
            wake(peer);
            return (long)len;
        }
        if (nonblock || (f->flags & AURORA_O_NONBLOCK)) {
            kfree(m);
            return AE_AGAIN;
        }
        if (task_interrupted(sched_current())) {
            kfree(m);
            return AE_INTR;
        }
        uint64_t fl = irq_save();
        sched_wait_on(peer, 0); /* woken when the peer dequeues or closes */
        irq_restore(fl);
    }
}

long local_recv(struct process *p, struct file *f, uint64_t ubuf, uint64_t len, uint64_t ufds, uint32_t *nfds,
                uint32_t *flags, bool nonblock)
{
    if (f->kind != FK_LOCAL)
        return AE_NOTSOCK;
    uint32_t room = nfds ? *nfds : 0;
    if (nfds)
        *nfds = 0;
    if (flags)
        *flags = 0;
    for (;;) {
        struct local_end *e = f->lend;
        struct local_msg *m = e->head;
        if (m) {
            uint64_t n = MIN(len, (uint64_t)m->len);
            if (n && copy_to_user(p, ubuf, m->data, n))
                return AE_FAULT;
            /* Install descriptors; ones that do not fit are closed. */
            int32_t got[AURORA_MSG_FDS];
            uint32_t k = 0;
            for (uint32_t i = 0; i < m->nfds; i++) {
                if (k < room) {
                    int fd = proc_fd_install(p, m->fds[i], 0, true);
                    if (fd >= 0) {
                        got[k++] = fd;
                        m->fds[i] = NULL;
                        continue;
                    }
                }
                if (flags)
                    *flags |= AURORA_MSG_TRUNC;
            }
            if (k && copy_to_user(p, ufds, got, k * sizeof got[0])) {
                for (uint32_t i = 0; i < k; i++)
                    proc_fd_close(p, got[i]);
                k = 0;
            }
            if (nfds)
                *nfds = k;
            if (n < m->len && flags)
                *flags |= AURORA_MSG_TRUNC;
            e->head = m->next;
            if (!e->head)
                e->tail = NULL;
            e->qbytes -= m->len;
            e->qmsgs--;
            for (uint32_t i = 0; i < m->nfds; i++)
                if (m->fds[i])
                    file_put(m->fds[i]);
            kfree(m);
            wake(e); /* a sender may be waiting for room */
            return (long)n;
        }
        if (!e->peer)
            return 0; /* peer closed and the queue is drained */
        if (nonblock || (f->flags & AURORA_O_NONBLOCK))
            return AE_AGAIN;
        if (task_interrupted(sched_current()))
            return AE_INTR;
        uint64_t fl = irq_save();
        if (!e->head && e->peer)
            sched_wait_on(e, 0);
        irq_restore(fl);
    }
}

int local_poll_end(struct local_end *e, int events)
{
    int r = 0;
    if (e->head)
        r |= AURORA_POLLIN;
    if (!e->peer) {
        r |= AURORA_POLLHUP | AURORA_POLLIN;
    } else if (e->peer->qmsgs < LOCAL_QUEUE_MSGS && e->peer->qbytes < LOCAL_QUEUE_BYTES) {
        r |= AURORA_POLLOUT;
    }
    return r & (events | AURORA_POLLHUP | AURORA_POLLERR);
}

int local_poll_listener(struct local_listener *l, int events)
{
    return (l->npending ? AURORA_POLLIN : 0) & events;
}
