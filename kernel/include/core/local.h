#ifndef AURORA_LOCAL_H
#define AURORA_LOCAL_H
#include "types.h"
#include "abi/aurora_abi.h"

/* Local message sockets (0.7): connected endpoint pairs that keep message
 * boundaries and can carry open descriptors. Listening sockets publish a
 * name in a small kernel namespace (not the VFS). Everything runs under the
 * BKL; blocking calls sleep on the endpoint and re-check afterwards.
 *
 * Limits: each endpoint queues at most LOCAL_QUEUE_BYTES bytes and
 * LOCAL_QUEUE_MSGS messages (senders block or get EAGAIN). Local sockets
 * themselves cannot be sent as descriptors, so no reference cycle can keep
 * an endpoint alive. */

#define LOCAL_QUEUE_BYTES (256u * 1024u)
#define LOCAL_QUEUE_MSGS 256u
#define LOCAL_BACKLOG_MAX 16
#define LOCAL_NAMES_MAX 32

struct file;
struct process;
struct local_end;
struct local_listener;

int local_pair(struct file **a, struct file **b);
int local_listen(const char *name, int backlog, struct file **out);
int local_connect(const char *name, struct file **out);
int local_accept(struct file *lf, bool nonblock, struct file **out);

/* User-facing send/receive. fds/nfds: descriptors of the caller. */
long local_send(struct process *p, struct file *f, uint64_t ubuf, uint64_t len, const int32_t *fds, uint32_t nfds,
                bool nonblock);
long local_recv(struct process *p, struct file *f, uint64_t ubuf, uint64_t len, uint64_t ufds, uint32_t *nfds,
                uint32_t *flags, bool nonblock);

void local_end_close(struct local_end *e);
void local_listener_close(struct local_listener *l);
int local_poll_end(struct local_end *e, int events);
int local_poll_listener(struct local_listener *l, int events);

#endif
