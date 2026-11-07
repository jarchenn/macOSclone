/* ELF64 executable validation: accepted images and every rejection path. */
#include "harness.h"
#include "core/elf.h"
/* Same values as AURORA_USER_BASE / AURORA_USER_LIMIT in abi/aurora_abi.h
 * (not included: the host C library defines a conflicting SYS_OPEN). */

const char *t_suite = "elf";

static uint8_t img[8192];

struct seg {
    uint32_t type, flags;
    uint64_t off, vaddr, filesz, memsz;
};

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }

static size_t build(uint64_t entry, const struct seg *s, int n)
{
    memset(img, 0, sizeof img);
    memcpy(img, "\x7f" "ELF", 4);
    img[4] = 2; img[5] = 1; img[6] = 1;
    put16(img + 16, 2);   /* ET_EXEC */
    put16(img + 18, 62);  /* x86-64 */
    put32(img + 20, 1);
    put64(img + 24, entry);
    put64(img + 32, 64);  /* phoff */
    put16(img + 52, 64);
    put16(img + 54, 56);
    put16(img + 56, (uint16_t)n);
    for (int i = 0; i < n; i++) {
        uint8_t *ph = img + 64 + 56 * i;
        put32(ph, s[i].type);
        put32(ph + 4, s[i].flags);
        put64(ph + 8, s[i].off);
        put64(ph + 16, s[i].vaddr);
        put64(ph + 24, s[i].vaddr);
        put64(ph + 32, s[i].filesz);
        put64(ph + 40, s[i].memsz);
        put64(ph + 48, 4096);
    }
    return sizeof img;
}

#define B 0x0000000000400000ull
#define L 0x0000700000000000ull

static int check(uint64_t entry, const struct seg *s, int n)
{
    struct elf_image out;
    size_t sz = build(entry, s, n);
    return elf_validate(img, sz, B, L, &out);
}

TEST(valid_three_segments)
{
    struct seg s[] = {
        { 1, 5, 4096, 0x400000, 100, 100 },   /* text RX */
        { 1, 4, 4096, 0x401000, 50, 50 },     /* rodata R (shares file page, different VA) */
        { 1, 6, 4096, 0x402000, 10, 9000 },   /* data+bss RW */
    };
    struct elf_image out;
    size_t sz = build(0x400010, s, 3);
    CHECK_EQ(elf_validate(img, sz, B, L, &out), ELF_OK);
    CHECK_EQ(out.nseg, 3);
    CHECK_EQ(out.entry, 0x400010);
    CHECK(out.seg[0].executable && !out.seg[0].writable);
    CHECK(out.seg[2].writable && !out.seg[2].executable);
    CHECK_EQ(out.seg[2].memsz, 9000);
}

TEST(header_rejections)
{
    struct seg ok[] = { { 1, 5, 4096, 0x400000, 100, 100 } };
    struct elf_image out;
    size_t sz = build(0x400000, ok, 1);
    CHECK_EQ(elf_validate(img, 10, B, L, &out), ELF_E_TRUNCATED);
    CHECK_EQ(elf_validate(NULL, 0, B, L, &out), ELF_E_TRUNCATED);
    img[1] = 'X';
    CHECK_EQ(elf_validate(img, sz, B, L, &out), ELF_E_MAGIC);
    build(0x400000, ok, 1);
    img[4] = 1; /* ELF32 */
    CHECK_EQ(elf_validate(img, sz, B, L, &out), ELF_E_CLASS);
    build(0x400000, ok, 1);
    img[16] = 3; /* ET_DYN (PIE / shared object) */
    CHECK_EQ(elf_validate(img, sz, B, L, &out), ELF_E_TYPE);
    build(0x400000, ok, 1);
    img[18] = 183; /* aarch64 */
    CHECK_EQ(elf_validate(img, sz, B, L, &out), ELF_E_MACHINE);
    build(0x400000, ok, 1);
    img[54] = 32; /* wrong phentsize */
    CHECK_EQ(elf_validate(img, sz, B, L, &out), ELF_E_PHDR);
    build(0x400000, ok, 1);
    put64(img + 32, sz - 10); /* phdrs past end of file */
    CHECK_EQ(elf_validate(img, sz, B, L, &out), ELF_E_PHDR);
}

TEST(segment_rejections)
{
    struct seg past_file[] = { { 1, 5, 4096, 0x400000, 9000, 9000 } };
    CHECK_EQ(check(0x400000, past_file, 1), ELF_E_SEGMENT);
    struct seg filesz_gt_memsz[] = { { 1, 5, 4096, 0x400000, 200, 100 } };
    CHECK_EQ(check(0x400000, filesz_gt_memsz, 1), ELF_E_SEGMENT);
    struct seg misaligned[] = { { 1, 5, 4096, 0x400010, 100, 100 } };
    CHECK_EQ(check(0x400010, misaligned, 1), ELF_E_SEGMENT);
    struct seg low[] = { { 1, 5, 4096, 0x1000, 100, 100 } }; /* below user base (NULL area) */
    CHECK_EQ(check(0x1000, low, 1), ELF_E_RANGE);
    struct seg kernel[] = { { 1, 5, 4096, 0xffffffff80000000ull, 100, 100 } };
    CHECK_EQ(check(0xffffffff80000000ull, kernel, 1), ELF_E_RANGE);
    struct seg wrap[] = { { 1, 6, 4096, 0x400000, 10, 0xfffffffffff00000ull } };
    CHECK_EQ(check(0x400000, wrap, 1), ELF_E_RANGE);
    struct seg wx[] = { { 1, 7, 4096, 0x400000, 100, 100 } };
    CHECK_EQ(check(0x400000, wx, 1), ELF_E_WX);
    struct seg overlap[] = { { 1, 5, 4096, 0x400000, 100, 5000 }, { 1, 6, 4096, 0x401000, 10, 10 } };
    CHECK_EQ(check(0x400000, overlap, 2), ELF_E_OVERLAP);
    struct seg big[] = { { 1, 6, 4096, 0x400000, 10, 32ull << 20 } };
    CHECK_EQ(check(0x400000, big, 1), ELF_E_TOOBIG);
    struct seg interp[] = { { 3, 4, 4096, 0x400000, 10, 10 }, { 1, 5, 4096, 0x400000, 10, 10 } };
    CHECK_EQ(check(0x400000, interp, 2), ELF_E_NOLOAD);
    struct seg none[] = { { 4, 4, 4096, 0x400000, 10, 10 } }; /* PT_NOTE only */
    CHECK_EQ(check(0x400000, none, 1), ELF_E_NOLOAD);
}

TEST(entry_must_be_executable)
{
    struct seg s[] = { { 1, 5, 4096, 0x400000, 100, 100 }, { 1, 6, 4096, 0x401000, 10, 10 } };
    CHECK_EQ(check(0x401000, s, 2), ELF_E_ENTRY);   /* entry in data */
    CHECK_EQ(check(0x500000, s, 2), ELF_E_ENTRY);   /* entry unmapped */
    CHECK_EQ(check(0x400063, s, 2), ELF_OK);
    CHECK_EQ(check(0x400064, s, 2), ELF_E_ENTRY);   /* one past the end */
    CHECK(strcmp(elf_strerror(ELF_E_WX), "segment both writable and executable") == 0);
}

void run_tests(void)
{
    RUN(valid_three_segments);
    RUN(header_rejections);
    RUN(segment_rejections);
    RUN(entry_must_be_executable);
}
