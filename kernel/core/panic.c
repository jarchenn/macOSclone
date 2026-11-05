/* Panic path: no heap, no locks, no complex rendering. Logs to serial and the
 * framebuffer diagnostic console, then halts with interrupts disabled. */
#include "core/panic.h"
#include "core/log.h"
#include "arch/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/serial.h"
#include "graphics/console.h"
#include "lib.h"

static volatile int panicking;
static char panic_buf[512];

static void out(const char *s)
{
    serial_write(s, strlen(s));
    console_write(s);
}

static void outf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(panic_buf, sizeof panic_buf, fmt, ap);
    va_end(ap);
    out(panic_buf);
}

static NORETURN void halt_forever(void)
{
    for (;;) {
        cpu_cli();
        cpu_hlt();
    }
}

static bool enter_panic(void)
{
    cpu_cli();
    if (__atomic_exchange_n(&panicking, 1, __ATOMIC_SEQ_CST)) {
        /* Recursive panic: emit the bare minimum and stop. */
        serial_write("\nAURORA_PANIC (recursive)\n", 26);
        return false;
    }
    klog_set_console(false);
    console_begin_panic();
    return true;
}

static void backtrace(uint64_t rbp)
{
    out("backtrace:\n");
    for (int i = 0; i < 16 && rbp; i++) {
        /* Only follow frames inside the kernel higher half and aligned. */
        if (rbp < 0xffff800000000000ull || (rbp & 7))
            break;
        uint64_t *frame = (uint64_t *)(uintptr_t)rbp;
        uint64_t ret = frame[1];
        if (!ret)
            break;
        outf("  #%d %016llx\n", i, (unsigned long long)ret);
        rbp = frame[0];
    }
}

void panic(const char *fmt, ...)
{
    if (!enter_panic())
        halt_forever();
    out("\n*** AURORA_PANIC ***\n");
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(panic_buf, sizeof panic_buf, fmt, ap);
    va_end(ap);
    out(panic_buf);
    out("\n");
    backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0));
    out("System halted.\n");
    halt_forever();
}

static const char *exception_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "BOUND range exceeded",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor segment overrun",
    "Invalid TSS", "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 FP exception", "Alignment check", "Machine check",
    "SIMD FP exception", "Virtualization exception", "Control protection", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Hypervisor injection",
    "VMM communication", "Security exception", "Reserved",
};

void panic_exception(const struct interrupt_frame *f)
{
    if (!enter_panic())
        halt_forever();
    uint64_t v = f->vector;
    out("\n*** AURORA_PANIC ***\n");
    outf("CPU exception %llu: %s\n", (unsigned long long)v, v < 32 ? exception_names[v] : "?");
    outf("error_code=%016llx  rip=%016llx  cs=%04llx  rflags=%016llx\n",
         (unsigned long long)f->error_code, (unsigned long long)f->rip,
         (unsigned long long)f->cs, (unsigned long long)f->rflags);
    if (v == 14) {
        uint64_t cr2 = read_cr2();
        uint64_t e = f->error_code;
        outf("CR2=%016llx  (%s, %s, %s%s%s)\n", (unsigned long long)cr2,
             (e & 1) ? "protection violation" : "page not present",
             (e & 2) ? "write" : "read",
             (e & 4) ? "user" : "supervisor",
             (e & 8) ? ", reserved bit set" : "",
             (e & 16) ? ", instruction fetch" : "");
        if (cr2 < 4096)
            out("hint: NULL pointer dereference\n");
    }
    outf("rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", (unsigned long long)f->rax,
         (unsigned long long)f->rbx, (unsigned long long)f->rcx, (unsigned long long)f->rdx);
    outf("rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n", (unsigned long long)f->rsi,
         (unsigned long long)f->rdi, (unsigned long long)f->rbp, (unsigned long long)f->rsp);
    outf("r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n", (unsigned long long)f->r8,
         (unsigned long long)f->r9, (unsigned long long)f->r10, (unsigned long long)f->r11);
    outf("r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n", (unsigned long long)f->r12,
         (unsigned long long)f->r13, (unsigned long long)f->r14, (unsigned long long)f->r15);
    outf("cr0=%016llx cr3=%016llx cr4=%016llx ss=%04llx\n", (unsigned long long)read_cr0(),
         (unsigned long long)read_cr3(), (unsigned long long)read_cr4(), (unsigned long long)f->ss);
    if (v != 8)
        backtrace(f->rbp);
    out("System halted.\n");
    halt_forever();
}
