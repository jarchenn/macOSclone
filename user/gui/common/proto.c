/* Display protocol message helpers (gui/proto.h). */
#include "types.h"
#include "gui/proto.h"
#include <aurora.h>
#include <errno.h>
#include <string.h>

int proto_send(int fd, uint16_t type, uint32_t win, const void *payload, size_t len, const int *fds, unsigned nfds)
{
    static uint8_t buf[PROTO_MAX_MSG];
    if (len > sizeof buf - sizeof(struct msg_hdr))
        return -1;
    struct msg_hdr h = { type, 0, win };
    memcpy(buf, &h, sizeof h);
    if (len)
        memcpy(buf + sizeof h, payload, len);
    for (;;) {
        long r = aurora_sendmsg(fd, buf, sizeof h + len, fds, nfds, 0);
        if (r >= 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

long proto_recv(int fd, void *buf, size_t cap, int *fds, unsigned *nfds, bool nonblock)
{
    unsigned room = nfds ? *nfds : 0;
    for (;;) {
        unsigned n = room, fl = 0;
        long r = aurora_recvmsg(fd, buf, cap, fds, &n, nonblock ? AURORA_MSG_DONTWAIT : 0, &fl);
        if (r >= 0) {
            if (nfds)
                *nfds = n;
            return r;
        }
        if (errno != EINTR)
            return -1;
    }
}
