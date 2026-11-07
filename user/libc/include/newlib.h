/* newlib configuration for Aurora (0.6). Written by hand in place of the
 * header newlib's configure script would generate (tools/userland.py builds
 * newlib without running its build system). */
#ifndef __NEWLIB_H__
#define __NEWLIB_H__ 1

#include <_newlib_version.h>

#define _ATEXIT_DYNAMIC_ALLOC 1
#define _ELIX_LEVEL 4
#define _FSEEK_OPTIMIZATION 1
#define _FVWRITE_IN_STREAMIO 1
#define _HAVE_INITFINI_ARRAY 1
#define _HAVE_LONG_DOUBLE 1
#define HAVE_LONG_DOUBLE 1
#define _MB_LEN_MAX 1
#define _RETARGETABLE_LOCKING 1
#define _UNBUF_STREAM_OPT 1
#define _WANT_IO_C99_FORMATS 1
#define _WANT_IO_LONG_DOUBLE 1
#define _WANT_IO_LONG_LONG 1
/* _WANT_IO_POS_ARGS is off: newlib takes the address of a va_list
 * parameter there, which is wrong where va_list is an array (x86-64). */
#define _WANT_REENT_THREAD_LOCAL 1 /* errno and friends are _Thread_local */
#define _WIDE_ORIENT 1

#endif
