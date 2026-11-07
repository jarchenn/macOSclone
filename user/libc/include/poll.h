#ifndef _POLL_H
#define _POLL_H
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned int nfds_t;
struct pollfd {
    int fd;
    short events, revents;
};
#define POLLIN 0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define POLLRDNORM POLLIN
#define POLLWRNORM POLLOUT
int poll(struct pollfd *fds, nfds_t n, int timeout_ms);
#ifdef __cplusplus
}
#endif
#endif
