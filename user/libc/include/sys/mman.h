#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01   /* not supported in 0.6: mmap fails with ENOTSUP */
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t len);
int mprotect(void *addr, size_t len, int prot);
#ifdef __cplusplus
}
#endif
#endif
