#ifndef AURORA_ELF_H
#define AURORA_ELF_H
#include "types.h"

/* ELF64 executable validation (portable, host-tested). Accepts only static
 * x86-64 ET_EXEC images whose PT_LOAD segments lie inside the user range,
 * do not overlap, are not both writable and executable, and whose entry
 * point is inside an executable segment. */
#define ELF_MAX_SEGMENTS 8
#define ELF_MAX_IMAGE_BYTES (16u * 1024u * 1024u)

enum elf_err {
    ELF_OK = 0,
    ELF_E_TRUNCATED = -1,
    ELF_E_MAGIC = -2,
    ELF_E_CLASS = -3,
    ELF_E_TYPE = -4,
    ELF_E_MACHINE = -5,
    ELF_E_PHDR = -6,
    ELF_E_SEGMENT = -7,
    ELF_E_RANGE = -8,
    ELF_E_OVERLAP = -9,
    ELF_E_WX = -10,
    ELF_E_ENTRY = -11,
    ELF_E_TOOBIG = -12,
    ELF_E_NOLOAD = -13,
};

struct elf_segment {
    uint64_t vaddr, memsz, offset, filesz;
    bool writable, executable;
};

struct elf_image {
    uint64_t entry;
    int nseg;
    struct elf_segment seg[ELF_MAX_SEGMENTS];
};

int elf_validate(const uint8_t *data, size_t size, uint64_t user_base, uint64_t user_limit, struct elf_image *out);
const char *elf_strerror(int err);

#endif
