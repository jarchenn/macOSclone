#include "core/elf.h"
#include "lib.h"

#define PT_LOAD 1
#define PT_INTERP 3
#define PT_DYNAMIC 2
#define PF_X 1
#define PF_W 2
#define PAGE 4096ull

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

const char *elf_strerror(int err)
{
    switch (err) {
    case ELF_OK: return "ok";
    case ELF_E_TRUNCATED: return "file truncated";
    case ELF_E_MAGIC: return "not an ELF file";
    case ELF_E_CLASS: return "not a little-endian ELF64 file";
    case ELF_E_TYPE: return "not a static executable (ET_EXEC)";
    case ELF_E_MACHINE: return "not an x86-64 executable";
    case ELF_E_PHDR: return "bad program header table";
    case ELF_E_SEGMENT: return "bad segment (sizes or alignment)";
    case ELF_E_RANGE: return "segment outside the user address range";
    case ELF_E_OVERLAP: return "overlapping segments";
    case ELF_E_WX: return "segment both writable and executable";
    case ELF_E_ENTRY: return "entry point not in an executable segment";
    case ELF_E_TOOBIG: return "image too large";
    case ELF_E_NOLOAD: return "no loadable segments, or dynamic linking required";
    default: return "invalid executable";
    }
}

int elf_validate(const uint8_t *d, size_t size, uint64_t base, uint64_t limit, struct elf_image *out)
{
    memset(out, 0, sizeof *out);
    if (!d || size < 64)
        return ELF_E_TRUNCATED;
    if (d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F')
        return ELF_E_MAGIC;
    if (d[4] != 2 || d[5] != 1 || d[6] != 1)
        return ELF_E_CLASS;
    if (rd16(d + 16) != 2)
        return ELF_E_TYPE;
    if (rd16(d + 18) != 62 || rd32(d + 20) != 1)
        return ELF_E_MACHINE;
    uint64_t entry = rd64(d + 24);
    uint64_t phoff = rd64(d + 32);
    uint16_t phentsize = rd16(d + 54);
    uint16_t phnum = rd16(d + 56);
    if (phentsize != 56 || phnum == 0 || phnum > 32)
        return ELF_E_PHDR;
    if (phoff > size || (uint64_t)phnum * phentsize > size - phoff)
        return ELF_E_PHDR;
    uint64_t total = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + (uint64_t)i * phentsize;
        uint32_t type = rd32(ph);
        if (type == PT_INTERP || type == PT_DYNAMIC)
            return ELF_E_NOLOAD;
        if (type != PT_LOAD)
            continue;
        uint32_t flags = rd32(ph + 4);
        uint64_t off = rd64(ph + 8), vaddr = rd64(ph + 16), filesz = rd64(ph + 32), memsz = rd64(ph + 40);
        if (memsz == 0)
            continue;
        if (out->nseg >= ELF_MAX_SEGMENTS)
            return ELF_E_SEGMENT;
        if (filesz > memsz || off > size || filesz > size - off)
            return ELF_E_SEGMENT;
        if ((vaddr % PAGE) != (off % PAGE))
            return ELF_E_SEGMENT;
        if (vaddr < base || memsz > limit || vaddr > limit - memsz)
            return ELF_E_RANGE;
        if ((flags & PF_W) && (flags & PF_X))
            return ELF_E_WX;
        total += ALIGN_UP(vaddr + memsz, PAGE) - ALIGN_DOWN(vaddr, PAGE);
        if (total > ELF_MAX_IMAGE_BYTES)
            return ELF_E_TOOBIG;
        struct elf_segment *s = &out->seg[out->nseg];
        /* Page-granular overlap check against earlier segments. */
        uint64_t s0 = ALIGN_DOWN(vaddr, PAGE), s1 = ALIGN_UP(vaddr + memsz, PAGE);
        for (int k = 0; k < out->nseg; k++) {
            uint64_t o0 = ALIGN_DOWN(out->seg[k].vaddr, PAGE);
            uint64_t o1 = ALIGN_UP(out->seg[k].vaddr + out->seg[k].memsz, PAGE);
            if (s0 < o1 && o0 < s1)
                return ELF_E_OVERLAP;
        }
        s->vaddr = vaddr;
        s->memsz = memsz;
        s->offset = off;
        s->filesz = filesz;
        s->writable = (flags & PF_W) != 0;
        s->executable = (flags & PF_X) != 0;
        out->nseg++;
    }
    if (out->nseg == 0)
        return ELF_E_NOLOAD;
    for (int k = 0; k < out->nseg; k++) {
        const struct elf_segment *s = &out->seg[k];
        if (s->executable && entry >= s->vaddr && entry < s->vaddr + s->memsz) {
            out->entry = entry;
            return ELF_OK;
        }
    }
    return ELF_E_ENTRY;
}
