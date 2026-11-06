/* Directory entries for Aurora (newlib's posix/ readdir code uses this
 * layout; records come from the getdents system call). */
#ifndef _SYS_DIRENT_H
#define _SYS_DIRENT_H

#include <sys/types.h>
#include <sys/lock.h>

#define HAVE_DD_LOCK 1
#define MAXNAMLEN 255

struct dirent {
    __uint64_t d_ino;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[MAXNAMLEN + 1]; /* records are shorter: d_reclen bytes */
};

typedef struct {
    int dd_fd;
    int dd_loc;
    int dd_size;
    char *dd_buf;
    int dd_len;
    long dd_seek;
    _LOCK_RECURSIVE_T dd_lock;
} DIR;

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

#define __dirfd(dp) ((dp)->dd_fd)

int getdents(int fd, void *buf, int len);

#endif
