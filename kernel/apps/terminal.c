/* Terminal: built-in commands plus a pseudo-terminal for programs (0.6).
 * Programs run as session leaders on the Terminal's pty: their output goes
 * through a small VT100-subset interpreter (CR, BS, TAB, CSI K/J/H/C/D/G/m)
 * into the scrollback, and while one runs in the foreground every key is
 * sent to the pty, whose line discipline echoes and edits (Ctrl-C there is
 * SIGINT for the foreground process group). */
#include "desktop/desktop.h"
#include "apps/cmdline.h"
#include "apps/textbuf.h"
#include "arch/percpu.h"
#include "core/spinlock.h"
#include "core/smptest.h"
#include "apps/app_util.h"
#include "fs/vfs.h"
#include "core/log.h"
#include "core/time.h"
#include "core/boot.h"
#include "memory/heap.h"
#include "memory/physical.h"
#include "drivers/framebuffer.h"
#include "core/process.h"
#include "core/sched.h"
#include "core/pty.h"
#include "net/net.h"
#include "lib.h"

#define TERM_LINES 400
#define TERM_COLS 120
#define HISTORY 32

enum { C_TEXT = 0, C_DIM, C_ERROR, C_PROMPT, C_ACCENT };

struct term_line {
    char text[TERM_COLS + 1];
    uint8_t color;
};

static struct {
    uint32_t win_id;
    struct term_line lines[TERM_LINES];
    int head, count;
    char input[CMD_LINE_MAX + 1];
    size_t in_len, in_cur;
    char history[HISTORY][CMD_LINE_MAX + 1];
    int hist_count, hist_pos;
    char cwd[VFS_PATH_MAX + 1];
    int scroll;
    bool caret;
    bool warned_full;
    int fg_pid; /* foreground process receiving keyboard input, 0 = none */
    struct pty *pty;
    /* Current (unterminated) output line and the VT100 parser state. */
    char line[TERM_COLS + 1];
    size_t line_n, line_cur;
    uint8_t line_color, attr;
    int esc;          /* 0 text, 1 after ESC, 2 in CSI */
    /* Type-ahead: input the foreground program left unread when it exited,
     * replayed as keys (to the Terminal or the next program) from tick(). */
    char pending[CMD_LINE_MAX * 2];
    size_t pend_n, pend_off;
    int csi[4], ncsi;
} T;

static int cols(void)
{
    struct window *w = wm_get(T.win_id);
    int c = w ? (w->content.w - 20) / 8 : 80;
    return CLAMP(c, 20, TERM_COLS);
}

static void push_line(const char *s, size_t n, uint8_t color)
{
    int idx = (T.head + T.count) % TERM_LINES;
    if (T.count == TERM_LINES)
        T.head = (T.head + 1) % TERM_LINES;
    else
        T.count++;
    if (n > TERM_COLS)
        n = TERM_COLS;
    memcpy(T.lines[idx].text, s, n);
    T.lines[idx].text[n] = '\0';
    T.lines[idx].color = color;
    TEST_EVENT("TERM %s", T.lines[idx].text);
}

static void out_color(uint8_t color, const char *text)
{
    int c = cols();
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len == 0) {
            if (nl)
                push_line("", 0, color);
        }
        size_t off = 0;
        while (off < len) {
            size_t n = MIN(len - off, (size_t)c);
            push_line(p + off, n, color);
            off += n;
        }
        if (!nl)
            break;
        p = nl + 1;
        if (!*p)
            break;
    }
    T.scroll = 0;
    wm_invalidate(wm_get(T.win_id));
}

static void outf(uint8_t color, const char *fmt, ...) PRINTF_LIKE(2, 3);
static void outf(uint8_t color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out_color(color, buf);
}

static int resolve(const char *arg, char *out)
{
    return vfs_normalize(T.cwd, arg, out, VFS_PATH_MAX + 1);
}

/* ---- Commands --------------------------------------------------------------- */

static void cmd_help(void)
{
    out_color(C_ACCENT, "Aurora Terminal - built-in commands (not a POSIX shell):");
    static const char *const lines[] = {
        "  help                 this list",
        "  clear                clear the screen (also Ctrl+L)",
        "  about                release and architecture information",
        "  uname [-a]           system identity",
        "  uptime               time since boot (PIT timer)",
        "  mem                  memory statistics",
        "  cpus                 per-CPU scheduler and interrupt counters",
        "  ls [path]            list a directory",
        "  cat <path>           print a file",
        "  echo <text>          print text",
        "  touch <path>         create an empty file",
        "  write <path> <text>  replace a file's content",
        "  mkdir <path>         create a directory",
        "  rm <path> / mv <a> <b>  delete / rename or move",
        "  sync / df            flush the data disk / show storage use",
        "  cd [path] / pwd      change / show the current directory",
        "  open <app|path>      terminal, files, notes, settings, about, or a path",
        "  theme <light|dark>   switch the desktop theme",
        "  run <prog> [args] [&] start a user program from /system/bin (& = background)",
        "  <prog> [args]        same as run, for programs in /system/bin",
        "  ps / kill <pid>      list / stop user processes",
        "  ifconfig / netstat   network interface / sockets; see ping, nslookup, fetch",
        "  reboot / poweroff    restart / switch off the machine (ACPI S5)",
        "Quote arguments with \"...\"; inside quotes \\n is a newline.",
        "PgUp/PgDn scroll, Up/Down history. Ctrl+C cancels the line or stops the",
        "foreground program; Ctrl+D sends it end-of-input.",
    };
    for (size_t i = 0; i < ARRAY_LEN(lines); i++)
        out_color(C_TEXT, lines[i]);
}

static void cmd_about(void)
{
    outf(C_ACCENT, "Aurora OS %s (%s build %s)", AURORA_VERSION, AURORA_VARIANT, AURORA_BUILD_ID);
    outf(C_TEXT, "Architecture: x86_64, single CPU, QEMU 'pc' reference machine");
    outf(C_TEXT, "Bootloader:   %s %s, Limine protocol base revision %llu", g_boot.bootloader_name,
         g_boot.bootloader_version, (unsigned long long)g_boot.base_revision);
    outf(C_TEXT, "Firmware:     %s", app_firmware_name());
    outf(C_TEXT, "Display:      %ux%u, %u bpp software framebuffer", g_fb.width, g_fb.height, g_fb.bpp);
    out_color(C_DIM, "Desktop apps run inside the kernel; programs in /system/bin run as");
    out_color(C_DIM, "isolated ring-3 processes (try: run hello).");
    out_color(C_DIM, app_storage_note());
}

static void cmd_uptime(void)
{
    uint64_t ms = time_ms();
    uint64_t s = ms / 1000;
    outf(C_TEXT, "up %u:%02u:%02u  (%llu ms, %llu PIT ticks, divisor %u)", (unsigned)(s / 3600),
         (unsigned)(s / 60 % 60), (unsigned)(s % 60), (unsigned long long)ms,
         (unsigned long long)time_ticks(), pit_divisor());
}

static void cmd_cpus(void)
{
    outf(C_ACCENT, "CPU APIC     TICKS  USER%%  IDLE%%  KERN%%  SWITCHES   IPIS  TLB-IN");
    for (uint32_t i = 0; i < g_ncpus; i++) {
        const struct cpu *c = &g_cpus[i];
        uint64_t t = c->ticks ? c->ticks : 1;
        outf(c->online ? C_TEXT : C_DIM, "%3u %4u %9llu  %4llu   %4llu   %4llu  %8llu %6llu  %6llu%s", c->index, c->apic_id,
             (unsigned long long)c->ticks, (unsigned long long)(c->user_ticks * 100 / t),
             (unsigned long long)(c->idle_ticks * 100 / t), (unsigned long long)(c->kernel_ticks * 100 / t),
             (unsigned long long)c->switches, (unsigned long long)c->ipis, (unsigned long long)c->tlb_shootdowns,
             c->online ? "" : "  (offline)");
        TEST_EVENT("CPU idx=%u apic=%u online=%d ticks=%llu user=%llu idle=%llu switches=%llu tlb=%llu", c->index,
                   c->apic_id, c->online ? 1 : 0, (unsigned long long)c->ticks, (unsigned long long)c->user_ticks,
                   (unsigned long long)c->idle_ticks, (unsigned long long)c->switches,
                   (unsigned long long)c->tlb_shootdowns);
    }
    outf(C_DIM, "%u CPU(s) online; BKL contended %llu times; lockdep %u classes, %u violation(s), %u long hold(s)",
         g_ncpus_online, (unsigned long long)bkl_contended(), lockdep_classes(), lockdep_violations(),
         lockdep_long_holds());
    TEST_EVENT("CPUS online=%u lockdep_violations=%u long_holds=%u", g_ncpus_online, lockdep_violations(),
               lockdep_long_holds());
}

static void cmd_mem(void)
{
    const struct pmm *p = pmm_state();
    struct heap_stats hs;
    kheap_stats(&hs);
    struct vfs_usage u;
    vfs_usage(&u);
    outf(C_TEXT, "Usable RAM detected:    %llu KiB (%llu pages)", (unsigned long long)(p->usable_pages * 4),
         (unsigned long long)p->usable_pages);
    outf(C_TEXT, "Physical pages free:    %llu KiB (%llu pages)", (unsigned long long)(p->free_pages * 4),
         (unsigned long long)p->free_pages);
    outf(C_TEXT, "Kernel heap mapped:     %zu KiB of %zu KiB limit", hs.mapped_bytes / 1024, hs.limit_bytes / 1024);
    outf(C_TEXT, "Kernel heap in use:     %zu KiB in %zu allocations (peak %zu KiB)", hs.used_bytes / 1024,
         hs.alloc_count, hs.peak_used_bytes / 1024);
    outf(C_TEXT, "Files (/home):          %u bytes cached in RAM in %u nodes", u.ram_bytes, u.ram_nodes);
    struct vfs_storage stg;
    vfs_storage(&stg);
    if (stg.persistent)
        outf(C_TEXT, "Data disk:              %u of %u KiB used, generation %llu, %llu commits",
             stg.used_blocks * 4, stg.total_blocks * 4, (unsigned long long)stg.generation,
             (unsigned long long)stg.commits);
    else
        outf(C_TEXT, "Data disk:              none (files in /home reset on restart)");
    uint32_t ev_drop, ev_coal;
    desktop_input_stats(&ev_drop, &ev_coal);
    outf(C_TEXT, "Input drops:            kbd %u, mouse %u, events %u (%u coalesced)",
         ps2_kbd_ring()->dropped, ps2_mouse_ring()->dropped, ev_drop, ev_coal);
}

static void cmd_ls(int argc, char **argv)
{
    char path[VFS_PATH_MAX + 1];
    int r = resolve(argc > 1 ? argv[1] : ".", path);
    if (r) {
        outf(C_ERROR, "ls: %s: %s", argc > 1 ? argv[1] : ".", vfs_strerror(r));
        return;
    }
    struct vfs_stat st;
    r = vfs_stat(path, &st);
    if (r) {
        outf(C_ERROR, "ls: %s: %s", path, vfs_strerror(r));
        return;
    }
    if (st.type == VFS_FILE) {
        outf(C_TEXT, "%-32s %8u%s", path, st.size, st.readonly ? "  read-only" : "");
        return;
    }
    struct vfs_dirent d;
    uint32_t i = 0;
    for (; vfs_readdir(path, i, &d) == VFS_OK; i++) {
        if (d.type == VFS_DIR)
            outf(C_ACCENT, "%s/%*s%s", d.name, (int)(33 - strlen(d.name)), "", d.readonly ? "      dir  read-only" : "      dir");
        else
            outf(C_TEXT, "%-33s %8u%s", d.name, d.size, d.readonly ? "  read-only" : "");
    }
    if (i == 0)
        out_color(C_DIM, "(empty directory)");
}

static void cmd_cat(int argc, char **argv)
{
    if (argc < 2) {
        out_color(C_ERROR, "usage: cat <path>");
        return;
    }
    for (int a = 1; a < argc; a++) {
        char path[VFS_PATH_MAX + 1];
        int r = resolve(argv[a], path);
        if (r == VFS_OK) {
            static char buf[RAMFS_MAX_FILE + 1];
            size_t len;
            r = vfs_read_file(path, buf, RAMFS_MAX_FILE, &len);
            if (r == VFS_OK) {
                uint32_t sum = crc32(buf, len); /* of the real bytes, before display clean-up */
                UNUSED(sum);
                buf[len] = '\0';
                for (size_t i = 0; i < len; i++)
                    if (buf[i] == '\0' || buf[i] == '\r' || buf[i] == '\t')
                        buf[i] = ' ';
                if (len)
                    out_color(C_TEXT, buf);
                TEST_EVENT("CAT path=%s len=%zu crc=%08x", path, len, sum);
                continue;
            }
        }
        outf(C_ERROR, "cat: %s: %s", argv[a], vfs_strerror(r));
        TEST_EVENT("CAT_ERR path=%s err=%d", argv[a], r);
    }
}

static void join_args(int from, int argc, char **argv, char *out, size_t cap)
{
    out[0] = '\0';
    for (int i = from; i < argc; i++) {
        if (i > from)
            str_append(out, " ", cap);
        str_append(out, argv[i], cap);
    }
}

static void cmd_write(int argc, char **argv)
{
    if (argc < 3) {
        out_color(C_ERROR, "usage: write <path> <text>");
        return;
    }
    char path[VFS_PATH_MAX + 1];
    int r = resolve(argv[1], path);
    if (r == VFS_OK) {
        char text[CMD_LINE_MAX + 1];
        join_args(2, argc, argv, text, sizeof text);
        r = vfs_write_file(path, text, strlen(text));
        if (r == VFS_OK) {
            outf(C_DIM, "wrote %zu bytes to %s", strlen(text), path);
            files_refresh_all();
            return;
        }
    }
    outf(C_ERROR, "write: %s: %s", argv[1], vfs_strerror(r));
}

static void cmd_touch_mkdir(int argc, char **argv, bool dir)
{
    if (argc < 2) {
        outf(C_ERROR, "usage: %s <path>", argv[0]);
        return;
    }
    char path[VFS_PATH_MAX + 1];
    int r = resolve(argv[1], path);
    if (r == VFS_OK)
        r = dir ? vfs_mkdir(path) : vfs_create(path);
    if (r)
        outf(C_ERROR, "%s: %s: %s", argv[0], argv[1], vfs_strerror(r));
    else
        files_refresh_all();
}

static void cmd_cd(int argc, char **argv)
{
    char path[VFS_PATH_MAX + 1];
    int r = resolve(argc > 1 ? argv[1] : "/home", path);
    struct vfs_stat st;
    if (r == VFS_OK)
        r = vfs_stat(path, &st);
    if (r == VFS_OK && st.type != VFS_DIR)
        r = VFS_ENOTDIR;
    if (r) {
        outf(C_ERROR, "cd: %s: %s", argc > 1 ? argv[1] : "/home", vfs_strerror(r));
        return;
    }
    str_copy(T.cwd, path, sizeof T.cwd);
}

static bool ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        char y = *b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

static void cmd_open(int argc, char **argv)
{
    if (argc < 2) {
        out_color(C_ERROR, "usage: open <terminal|files|notes|settings|about|path>");
        return;
    }
    const char *a = argv[1];
    if (ieq(a, "terminal")) { terminal_open(); return; }
    if (ieq(a, "files")) { files_open(NULL); return; }
    if (ieq(a, "notes")) { notes_open(); return; }
    if (ieq(a, "settings")) { settings_open(0); return; }
    if (ieq(a, "about")) { settings_open(2); return; }
    if (ieq(a, "welcome")) { welcome_open(); return; }
    char path[VFS_PATH_MAX + 1];
    struct vfs_stat st;
    int r = resolve(a, path);
    if (r == VFS_OK)
        r = vfs_stat(path, &st);
    if (r) {
        outf(C_ERROR, "open: %s: %s", a, vfs_strerror(r));
        return;
    }
    if (st.type == VFS_DIR)
        files_open(path);
    else
        notes_open_path(path);
}

static void cmd_theme(int argc, char **argv)
{
    if (argc < 2 || (!ieq(argv[1], "light") && !ieq(argv[1], "dark"))) {
        out_color(C_ERROR, "usage: theme <light|dark>");
        return;
    }
    theme_set(ieq(argv[1], "dark"));
    desktop_theme_changed();
    outf(C_DIM, "theme set to %s", g_theme->dark ? "dark" : "light");
}

/* ---- Processes -------------------------------------------------------- */

static void flush_line(void)
{
    if (T.line_n) {
        push_line(T.line, T.line_n, T.line_color);
        T.line_n = T.line_cur = 0;
        T.line_color = T.attr;
    }
}

static void put_char(char ch, size_t c)
{
    if (T.line_cur >= c || T.line_cur >= TERM_COLS) {
        flush_line(); /* wrap */
    }
    T.line[T.line_cur++] = ch;
    if (T.line_cur > T.line_n)
        T.line_n = T.line_cur;
    if (T.attr != C_TEXT)
        T.line_color = T.attr;
}

static void csi_final(char f)
{
    int n = T.csi[0];
    switch (f) {
    case 'K': /* erase in line */
        if (n == 0)
            T.line_n = T.line_cur;
        else if (n == 1)
            for (size_t i = 0; i < T.line_cur && i < T.line_n; i++)
                T.line[i] = ' ';
        else
            T.line_n = T.line_cur = 0;
        break;
    case 'J':
        if (n == 2) {
            T.count = T.head = 0;
            T.line_n = T.line_cur = 0;
        }
        break;
    case 'H':
    case 'f':
    case 'G': {
        int col = (f == 'G' ? T.csi[0] : T.csi[1]);
        T.line_cur = col > 0 ? (size_t)(col - 1) : 0;
        if (T.line_cur > TERM_COLS - 1)
            T.line_cur = TERM_COLS - 1;
        while (T.line_n < T.line_cur)
            T.line[T.line_n++] = ' ';
        break;
    }
    case 'C':
        for (int k = 0; k < (n ? n : 1) && T.line_cur < TERM_COLS - 1; k++) {
            T.line_cur++;
            while (T.line_n < T.line_cur)
                T.line[T.line_n++] = ' ';
        }
        break;
    case 'D':
        T.line_cur = T.line_cur > (size_t)(n ? n : 1) ? T.line_cur - (size_t)(n ? n : 1) : 0;
        break;
    case 'm':
        for (int i = 0; i < (T.ncsi ? T.ncsi : 1); i++) {
            int a = T.csi[i];
            if (a == 0) T.attr = C_TEXT;
            else if (a == 2 || a == 90) T.attr = C_DIM;
            else if (a == 31 || a == 91) T.attr = C_ERROR;
            else if (a == 32 || a == 92) T.attr = C_PROMPT;
            else if ((a >= 33 && a <= 36) || (a >= 93 && a <= 96)) T.attr = C_ACCENT;
        }
        break;
    default:
        break; /* cursor up/down, modes: no effect in a line-based view */
    }
}

/* pty output callback (runs in the writer's system call, under the BKL). */
static void pty_output(void *ctx, const char *data, size_t len)
{
    UNUSED(ctx);
    size_t c = (size_t)cols();
    for (size_t k = 0; k < len; k++) {
        char ch = data[k];
        if (T.esc == 1) {
            if (ch == '[') {
                T.esc = 2;
                T.ncsi = 0;
                memset(T.csi, 0, sizeof T.csi);
            } else {
                T.esc = 0;
            }
            continue;
        }
        if (T.esc == 2) {
            if (ch >= '0' && ch <= '9') {
                if (T.ncsi == 0)
                    T.ncsi = 1;
                T.csi[T.ncsi - 1] = T.csi[T.ncsi - 1] * 10 + (ch - '0');
            } else if (ch == ';') {
                if (T.ncsi == 0)
                    T.ncsi = 1;
                if (T.ncsi < 4)
                    T.ncsi++;
            } else if (ch >= 0x40 && ch <= 0x7e) {
                csi_final(ch);
                T.esc = 0;
            }
            continue;
        }
        switch (ch) {
        case 0x1b: T.esc = 1; break;
        case '\n':
            if (T.line_n == 0)
                push_line("", 0, T.attr);
            else
                flush_line();
            T.line_n = T.line_cur = 0;
            break;
        case '\r': T.line_cur = 0; break;
        case '\b': if (T.line_cur) T.line_cur--; break;
        case '\t':
            do
                put_char(' ', c);
            while (T.line_cur % 8);
            break;
        case '\a': break;
        default:
            put_char((unsigned char)ch < 32 || (unsigned char)ch > 126 ? '?' : ch, c);
            break;
        }
    }
    T.scroll = 0;
    wm_invalidate(wm_get(T.win_id));
}

static void proc_exited(void *ctx, int pid, const char *name, int status, const char *reason)
{
    UNUSED(ctx);
    if (pid == T.fg_pid) {
        if (T.pty) {
            char tmp[CMD_LINE_MAX * 2];
            size_t n = pty_take_unread(T.pty, tmp, sizeof tmp);
            /* The unfinished part was echoed onto the current output line;
             * it will be shown again as Terminal input, so drop the echo. */
            size_t last = n;
            while (last > 0 && tmp[last - 1] != '\n')
                last--;
            size_t tail = n - last;
            if (tail && T.line_n >= tail && !memcmp(T.line + T.line_n - tail, tmp + last, tail)) {
                T.line_n -= tail;
                if (T.line_cur > T.line_n)
                    T.line_cur = T.line_n;
            }
            if (T.pend_off == T.pend_n)
                T.pend_off = T.pend_n = 0;
            for (size_t i = 0; i < n && T.pend_n < sizeof T.pending; i++)
                T.pending[T.pend_n++] = tmp[i];
        }
        flush_line();
        T.fg_pid = 0;
        if (T.pty)
            T.pty->fg_pgrp = 0;
    }
    if (reason && reason[0])
        outf(C_ERROR, "[%s (pid %d) %s]", name, pid, reason);
    else if ((status & 0x7f) == 0)
        outf(C_DIM, "[%s (pid %d) exited with status %d]", name, pid, (status >> 8) & 0xff);
    else
        outf(C_ERROR, "[%s (pid %d) killed by signal %d]", name, pid, status & 0x7f);
    wm_invalidate(wm_get(T.win_id));
}

static const struct proc_watch term_watch = { proc_exited, NULL };
static const struct pty_master term_master = { pty_output, NULL };

static int visible_rows(void)
{
    struct window *w = wm_get(T.win_id);
    return w ? MAX((w->content.h - 12) / 16, 1) : 24;
}

static struct pty *term_pty(void)
{
    if (!T.pty)
        T.pty = pty_create(&term_master, (uint16_t)visible_rows(), (uint16_t)cols());
    return T.pty;
}

static bool find_program(const char *name, char *path)
{
    struct vfs_stat st;
    if (strchr(name, '/'))
        return resolve(name, path) == VFS_OK && vfs_stat(path, &st) == VFS_OK && st.type == VFS_FILE;
    char tmp[VFS_PATH_MAX + 1];
    k_snprintf(tmp, sizeof tmp, "/system/bin/%s", name);
    if (vfs_stat(tmp, &st) == VFS_OK && st.type == VFS_FILE) {
        str_copy(path, tmp, VFS_PATH_MAX + 1);
        return true;
    }
    return false;
}

static void run_program(int argc, char **argv, int first)
{
    if (argc <= first) {
        out_color(C_ERROR, "usage: run <program> [args...] [&]");
        return;
    }
    bool background = argc > first + 1 && !strcmp(argv[argc - 1], "&");
    if (background)
        argc--;
    char path[VFS_PATH_MAX + 1];
    if (!find_program(argv[first], path)) {
        outf(C_ERROR, "run: %s: program not found (see ls /system/bin)", argv[first]);
        return;
    }
    struct pty *pt = term_pty();
    if (!pt) {
        out_color(C_ERROR, "run: cannot create a terminal device (out of memory)");
        return;
    }
    const char *av[CMD_MAX_ARGS + 1];
    int n = 0;
    for (int i = first; i < argc && n < CMD_MAX_ARGS; i++)
        av[n++] = argv[i];
    av[n] = NULL;
    char pwd[VFS_PATH_MAX + 8];
    k_snprintf(pwd, sizeof pwd, "PWD=%s", T.cwd);
    const char *envp[] = { "PATH=/system/bin", "HOME=/home", "TERM=vt100", "USER=root", "SHELL=/system/bin/sh",
                           pwd, NULL };
    int prev_fg = pt->fg_pgrp;
    struct proc_start ps = { .path = path, .argv = av, .envp = envp, .tty = pt, .watch = &term_watch,
                             .uid = 0, .gid = 0, .cwd = T.cwd };
    int pid = process_start(&ps);
    if (pid < 0) {
        outf(C_ERROR, "run: %s: %s", argv[first], process_spawn_error(pid));
        return;
    }
    if (background) {
        pt->fg_pgrp = prev_fg; /* keyboard signals stay with the foreground job */
        outf(C_DIM, "[%d] %s started in the background", pid, argv[first]);
    } else {
        T.fg_pid = pid;
        T.attr = T.line_color = C_TEXT;
    }
}

static void cmd_ps(void)
{
    outf(C_ACCENT, "  PID  PPID  UID THR STATE    CPU ms  MEM KiB  SYSCALLS  NAME");
    int n = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        struct process *p = process_at(i);
        if (!p)
            continue;
        uint64_t ms = process_cpu_ticks(p) * 1000 / PIT_TARGET_HZ;
        uint64_t kib = p->state == PROC_RUNNING ? mm_resident_pages(&p->mm) * 4 : 0;
        outf(C_TEXT, "%5d %5d %4u %3d %-7s %7llu  %7llu  %8llu  %s %s", p->pid, p->parent, p->uid, p->nthreads,
             p->state == PROC_ZOMBIE ? "zombie" : "running", (unsigned long long)ms, (unsigned long long)kib,
             (unsigned long long)p->syscalls, p->name, p->args);
        n++;
    }
    if (!n)
        out_color(C_DIM, "  (no user processes)");
    char tasks[160] = "kernel tasks:";
    for (int i = 0; i < sched_task_count(); i++) {
        struct task *t = sched_task_at(i);
        if (t && !t->proc) {
            str_append(tasks, " ", sizeof tasks);
            str_append(tasks, t->name, sizeof tasks);
        }
    }
    out_color(C_DIM, tasks);
    outf(C_DIM, "context switches: %llu", (unsigned long long)sched_context_switches());
}

static void cmd_kill(int argc, char **argv)
{
    if (argc < 2) {
        out_color(C_ERROR, "usage: kill <pid>");
        return;
    }
    int pid = 0;
    for (const char *q = argv[1]; *q >= '0' && *q <= '9'; q++)
        pid = pid * 10 + (*q - '0');
    if (process_kill(pid))
        outf(C_ERROR, "kill: %s: no such process", argv[1]);
}

/* Lines using shell syntax (pipes, redirection, lists, variables, globs)
 * go to /system/bin/sh -c; the built-in commands do not interpret them. */
static bool needs_shell(const char *line)
{
    for (const char *p = line; *p; p++)
        if (strchr("|<>;$*", *p) || (p[0] == '&' && p[1] == '&'))
            return true;
    return false;
}

static void execute(const char *line)
{
    if (needs_shell(line)) {
        char *argv[] = { "sh", "-c", (char *)line };
        run_program(3, argv, 0);
        return;
    }
    struct cmdline cl;
    int r = cmdline_parse(line, &cl);
    if (r) {
        outf(C_ERROR, "error: %s", cmdline_strerror(r));
        return;
    }
    if (cl.argc == 0)
        return;
    const char *c = cl.argv[0];
    if (!strcmp(c, "help")) cmd_help();
    else if (!strcmp(c, "clear")) { T.count = 0; T.head = 0; }
    else if (!strcmp(c, "about")) cmd_about();
    else if (!strcmp(c, "uname")) {
        if (cl.argc > 1 && !strcmp(cl.argv[1], "-a"))
            outf(C_TEXT, "Aurora %s x86_64 %s-build %s (preemptive, isolated processes)", AURORA_VERSION, AURORA_VARIANT, AURORA_BUILD_ID);
        else
            out_color(C_TEXT, "Aurora");
    }
    else if (!strcmp(c, "uptime")) cmd_uptime();
    else if (!strcmp(c, "mem")) cmd_mem();
    else if (!strcmp(c, "cpus")) cmd_cpus();
    else if (!strcmp(c, "ls")) cmd_ls(cl.argc, cl.argv);
    else if (!strcmp(c, "cat")) cmd_cat(cl.argc, cl.argv);
    else if (!strcmp(c, "echo")) {
        char text[CMD_LINE_MAX + 1];
        join_args(1, cl.argc, cl.argv, text, sizeof text);
        out_color(C_TEXT, text);
    }
    else if (!strcmp(c, "touch")) cmd_touch_mkdir(cl.argc, cl.argv, false);
    else if (!strcmp(c, "mkdir")) cmd_touch_mkdir(cl.argc, cl.argv, true);
    else if (!strcmp(c, "write")) cmd_write(cl.argc, cl.argv);
    else if (!strcmp(c, "cd")) cmd_cd(cl.argc, cl.argv);
    else if (!strcmp(c, "pwd")) out_color(C_TEXT, T.cwd);
    else if (!strcmp(c, "open")) cmd_open(cl.argc, cl.argv);
    else if (!strcmp(c, "theme")) cmd_theme(cl.argc, cl.argv);
#ifdef AURORA_TEST
    /* Test-build only: TLB shootdown test ("tlbtest noflush" = control run
     * without a shootdown, which may show a stale translation). */
    else if (!strcmp(c, "tlbtest")) {
        bool flush = !(cl.argc == 2 && !strcmp(cl.argv[1], "noflush"));
        struct tlb_test_result tr;
        int tres = smp_tlb_test(flush, &tr);
        if (tres)
            outf(C_ERROR, "tlbtest: could not run (%d)", tres);
        else
            outf(C_TEXT, "tlbtest: %s; cpu %d unmapped, cpu %d read before=%s after=%s", flush ? "shootdown" : "no shootdown",
                 tr.unmap_cpu, tr.reader_cpu, tr.first_read_ok ? "ok" : "BAD",
                 tr.second_read_faulted ? "fault" : (tr.second_value_stale ? "stale value" : "other"));
        TEST_EVENT("TLBTEST flush=%d r=%d unmap_cpu=%d reader_cpu=%d before=%d faulted=%d stale=%d", flush ? 1 : 0, tres,
                   tr.unmap_cpu, tr.reader_cpu, tr.first_read_ok ? 1 : 0, tr.second_read_faulted ? 1 : 0,
                   tr.second_value_stale ? 1 : 0);
    }
    /* Test-build only: rewrite /home/stress/f0..f7 for N rounds; every file
     * carries its own CRC so a later check can detect torn or mixed data. */
    else if (!strcmp(c, "pfsstress") && cl.argc == 2) {
        long rounds = 0;
        for (const char *q = cl.argv[1]; *q >= '0' && *q <= '9'; q++)
            rounds = rounds * 10 + (*q - '0');
        vfs_mkdir("/home/stress");
        static char body[4200];
        for (long rr = 1; rr <= rounds; rr++) {
            for (int k = 0; k < 8; k++) {
                int n = k_snprintf(body, sizeof body, "round=%ld file=%d ", rr, k);
                int pad = (int)((k * 517 + rr * 131) % 4000);
                for (int i = 0; i < pad; i++)
                    body[n++] = (char)('a' + (i + rr) % 26);
                n += k_snprintf(body + n, sizeof body - (size_t)n, " sum=%08x", crc32(body, (size_t)n));
                char path[40];
                k_snprintf(path, sizeof path, "/home/stress/f%d", k);
                int wr = vfs_write_file(path, body, (size_t)n);
                if (wr) {
                    outf(C_ERROR, "pfsstress: %s: %s", path, vfs_strerror(wr));
                    rr = rounds;
                    break;
                }
            }
            TEST_EVENT("PFS_ROUND %ld", rr);
        }
        outf(C_DIM, "pfsstress: done");
    }
    else if (!strcmp(c, "pfscheck")) {
        static char body[4200];
        int ok = 0, bad = 0, missing = 0;
        long maxround = 0, minround = 1L << 30;
        for (int k = 0; k < 8; k++) {
            char path[40];
            k_snprintf(path, sizeof path, "/home/stress/f%d", k);
            size_t len = 0;
            if (vfs_read_file(path, body, sizeof body - 1, &len)) {
                missing++;
                continue;
            }
            body[len] = 0;
            char *sum = len > 13 ? body + len - 13 : NULL;
            unsigned want = 0;
            bool good = sum && !strncmp(sum, " sum=", 5);
            for (int i = 5; good && i < 13; i++) {
                char ch = sum[i];
                int d = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
                if (d < 0)
                    good = false;
                want = want * 16 + (unsigned)d;
            }
            if (good && crc32(body, len - 13) == want) {
                long rr = 0;
                for (const char *q = body + 6; *q >= '0' && *q <= '9'; q++)
                    rr = rr * 10 + (*q - '0');
                maxround = MAX(maxround, rr);
                minround = MIN(minround, rr);
                ok++;
            } else {
                bad++;
            }
        }
        if (!ok)
            minround = 0;
        outf(bad ? C_ERROR : C_TEXT, "pfscheck: ok=%d bad=%d missing=%d rounds=%ld..%ld", ok, bad, missing, minround, maxround);
        TEST_EVENT("PFS_CHECK ok=%d bad=%d missing=%d min=%ld max=%ld", ok, bad, missing, minround, maxround);
    }
    /* Test-build only: create a file of N bytes ('a'..'z' pattern, newline
     * every 64 bytes) so capacity limits can be exercised quickly. */
    else if (!strcmp(c, "testfill") && cl.argc == 3) {
        static char buf[RAMFS_MAX_FILE + 1];
        uint32_t n = 0;
        for (const char *q = cl.argv[2]; *q >= '0' && *q <= '9'; q++)
            n = n * 10 + (uint32_t)(*q - '0');
        if (n > sizeof buf)
            n = sizeof buf;
        for (uint32_t i = 0; i < n; i++)
            buf[i] = (i % 64 == 63) ? '\n' : (char)('a' + i % 26);
        char path[VFS_PATH_MAX + 1];
        int fr = resolve(cl.argv[1], path);
        if (fr == VFS_OK)
            fr = vfs_write_file(path, buf, n);
        outf(fr ? C_ERROR : C_DIM, "testfill: %s %u bytes: %s", cl.argv[1], n, vfs_strerror(fr));
    }
#endif
    else if (!strcmp(c, "poweroff") || !strcmp(c, "shutdown")) {
        out_color(C_DIM, "powering off...");
        desktop_request_poweroff();
    }
    else if (!strcmp(c, "reboot")) {
        out_color(C_DIM, "restarting...");
        desktop_request_reboot();
    }
    else if (!strcmp(c, "rm") || !strcmp(c, "rmdir")) {
        if (cl.argc < 2) {
            outf(C_ERROR, "usage: %s <path>", c);
        } else {
            char path[VFS_PATH_MAX + 1];
            int er = resolve(cl.argv[1], path);
            if (er == VFS_OK)
                er = vfs_remove(path);
            if (er)
                outf(C_ERROR, "%s: %s: %s", c, cl.argv[1], vfs_strerror(er));
            files_refresh_all();
        }
    }
    else if (!strcmp(c, "mv")) {
        if (cl.argc < 3) {
            out_color(C_ERROR, "usage: mv <from> <to>");
        } else {
            char from[VFS_PATH_MAX + 1], to[VFS_PATH_MAX + 1];
            int er = resolve(cl.argv[1], from);
            if (er == VFS_OK)
                er = resolve(cl.argv[2], to);
            struct vfs_stat st;
            if (er == VFS_OK && vfs_stat(to, &st) == VFS_OK && st.type == VFS_DIR) {
                const char *base = strrchr(from, '/');
                str_append(to, base, sizeof to); /* move into the directory */
            }
            if (er == VFS_OK)
                er = vfs_rename(from, to);
            if (er)
                outf(C_ERROR, "mv: %s", vfs_strerror(er));
            files_refresh_all();
        }
    }
    else if (!strcmp(c, "sync")) {
        int er = vfs_sync();
        if (er)
            outf(C_ERROR, "sync: %s", vfs_strerror(er));
        else
            out_color(C_DIM, vfs_home_persistent() ? "sync: all changes are on the data disk"
                                                   : "sync: no data disk (nothing to flush)");
    }
    else if (!strcmp(c, "df")) {
        struct vfs_storage stg;
        vfs_storage(&stg);
        struct vfs_usage u;
        vfs_usage(&u);
        if (stg.persistent)
            outf(C_TEXT, "/home  data disk  %u KiB used of %u KiB  (generation %llu, %u corrupt files skipped)",
                 stg.used_blocks * 4, stg.total_blocks * 4, (unsigned long long)stg.generation, stg.corrupt_files);
        else
            outf(C_TEXT, "/home  RAM        %u KiB used of %u KiB (lost on restart)", u.ram_bytes / 1024, RAMFS_MAX_TOTAL / 1024);
        out_color(C_TEXT, "/system  initramfs  read-only");
    }
    else if (!strcmp(c, "ifconfig")) {
        const struct net_config *nc = net_info();
        if (!nc->up) {
            out_color(C_TEXT, "net0: no network card (start QEMU with a virtio-net device)");
        } else {
            char ip[16], mask[16], gw[16], dns[16];
            net_format_ip(nc->ip, ip, sizeof ip);
            net_format_ip(nc->netmask, mask, sizeof mask);
            net_format_ip(nc->gateway, gw, sizeof gw);
            net_format_ip(nc->dns, dns, sizeof dns);
            outf(C_TEXT, "net0: %s  mac %02x:%02x:%02x:%02x:%02x:%02x  %s", nc->driver, nc->mac[0], nc->mac[1],
                 nc->mac[2], nc->mac[3], nc->mac[4], nc->mac[5], nc->configured ? "configured (DHCP)" : "not configured");
            if (nc->configured)
                outf(C_TEXT, "      inet %s  netmask %s  gateway %s  dns %s", ip, mask, gw, dns);
            outf(C_TEXT, "      rx %llu packets (%llu dropped)  tx %llu packets (%llu errors)",
                 (unsigned long long)nc->rx_packets, (unsigned long long)nc->rx_dropped,
                 (unsigned long long)nc->tx_packets, (unsigned long long)nc->tx_errors);
        }
    }
    else if (!strcmp(c, "netstat")) {
        outf(C_TEXT, "%d socket(s) open", sock_count());
        out_color(C_DIM, "network commands: ping, nslookup, fetch, tcpecho, udpecho (in /system/bin)");
    }
    else if (!strcmp(c, "run")) run_program(cl.argc, cl.argv, 1);
    else if (!strcmp(c, "ps")) cmd_ps();
    else if (!strcmp(c, "kill")) cmd_kill(cl.argc, cl.argv);
    else {
        char path[VFS_PATH_MAX + 1];
        if (find_program(c, path))
            run_program(cl.argc, cl.argv, 0);
        else
            outf(C_ERROR, "%s: command not found (try 'help')", c);
    }
}

/* ---- Window callbacks ----------------------------------------------------- */

static uint32_t color_of(uint8_t c)
{
    const struct theme *t = g_theme;
    switch (c) {
    case C_DIM: return t->text_dim;
    case C_ERROR: return t->term_error;
    case C_PROMPT: return t->term_prompt;
    case C_ACCENT: return t->accent;
    default: return t->term_text;
    }
}

static void render(struct window *w, struct surface *s)
{
    const struct theme *t = g_theme;
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->term_bg);
    int rows = (s->h - 12) / 16;
    int c = cols();
    /* The prompt line(s) occupy the bottom of the history view. */
    char prompt[VFS_PATH_MAX + 16];
    bool fg = T.fg_pid && process_get(T.fg_pid);
    if (fg) {
        /* A foreground program owns the input: show its current line. */
        k_snprintf(prompt, sizeof prompt, "%.*s", (int)MIN(T.line_n, sizeof prompt - 1), T.line);
    } else
        k_snprintf(prompt, sizeof prompt, "aurora:%s$ ", T.cwd);
    size_t plen = strlen(prompt);
    size_t total = plen + (fg ? 0 : T.in_len);
    int prompt_rows = (int)(total / (size_t)c) + 1;
    int out_rows = rows - prompt_rows;
    int last = T.count - T.scroll;
    int first = last - out_rows;
    if (first < 0)
        first = 0;
    int y = 8;
    /* When there is less output than space, the prompt follows the output. */
    for (int i = first; i < last; i++) {
        const struct term_line *ln = &T.lines[(T.head + i) % TERM_LINES];
        draw_text(s, FONT, 10, y, ln->text, color_of(ln->color));
        y += 16;
    }
    if (T.scroll > 0) {
        draw_text(s, FONT, s->w - 10 - 18 * 8, 8, "-- scrolled back --", t->text_dim);
        return;
    }
    /* prompt + input, wrapped */
    char full[VFS_PATH_MAX + 16 + CMD_LINE_MAX + 1];
    str_copy(full, prompt, sizeof full);
    if (!fg)
        str_append(full, T.input, sizeof full);
    size_t caret = fg ? MIN(T.line_cur, plen) : plen + T.in_cur;
    bool focused = wm_focused() == w;
    for (int r = 0; r < prompt_rows; r++) {
        size_t off = (size_t)r * (size_t)c;
        size_t n = total > off ? MIN((size_t)c, total - off) : 0;
        int32_t x = 10;
        for (size_t i = 0; i < n; i++) {
            size_t idx = off + i;
            char ch[2] = { full[idx], 0 };
            uint32_t col = fg ? color_of(T.line_color) : idx < 7 ? t->term_prompt : idx < plen ? t->accent : t->term_text;
            draw_text(s, FONT, x, y, ch, col);
            x += 8;
        }
        if (focused && caret >= off && caret < off + (size_t)c) {
            int32_t cx = 10 + (int32_t)(caret - off) * 8;
            if (T.caret)
                draw_fill(s, rect_make(cx, y, 8, 16), t->term_prompt);
            if (caret < total && T.caret) {
                char ch[2] = { full[caret], 0 };
                draw_text(s, FONT, cx, y, ch, t->term_bg);
            }
        }
        y += 16;
    }
}

static void history_add(const char *line)
{
    if (!line[0])
        return;
    if (T.hist_count && !strcmp(T.history[(T.hist_count - 1) % HISTORY], line))
        return;
    str_copy(T.history[T.hist_count % HISTORY], line, sizeof T.history[0]);
    T.hist_count++;
}

static void history_recall(int dir)
{
    int avail = MIN(T.hist_count, HISTORY);
    if (!avail)
        return;
    if (T.hist_pos < 0)
        T.hist_pos = 0;
    T.hist_pos += dir;
    if (T.hist_pos > avail)
        T.hist_pos = avail;
    if (T.hist_pos <= 0) {
        T.hist_pos = 0;
        T.input[0] = '\0';
    } else {
        str_copy(T.input, T.history[(T.hist_count - T.hist_pos) % HISTORY], sizeof T.input);
    }
    T.in_len = T.in_cur = strlen(T.input);
}

/* While a program runs in the foreground, keys become terminal input. */
static bool fg_key(struct window *w, const struct key_event *e)
{
    if (!T.fg_pid)
        return false;
    if (!process_get(T.fg_pid) || !T.pty) {
        T.fg_pid = 0;
        return false;
    }
    if (e->key == KEY_PGUP || e->key == KEY_PGDN)
        return false; /* scrollback stays with the Terminal */
    char seq[8];
    size_t n = 0;
    if ((e->mods & MOD_CTRL) && e->key == KEY_CHAR && (e->ascii | 0x20) == 'v') {
        size_t len;
        const char *clip = clipboard_get(&len);
        size_t k = 0;
        while (k < len && clip[k] != '\n' && clip[k] != '\r')
            k++;
        pty_input(T.pty, clip, k);
        TEST_EVENT("TERM_PASTE added=%zu len=%zu", k, k);
        wm_invalidate(w);
        return true;
    }
    if ((e->mods & MOD_CTRL) && e->key == KEY_CHAR && e->ascii) {
        char ch = (char)(e->ascii | 0x20);
        if (ch >= 'a' && ch <= 'z')
            seq[n++] = (char)(ch - 'a' + 1);
        else if (e->ascii == '\\')
            seq[n++] = 0x1c;
    } else {
        switch (e->key) {
        case KEY_ENTER: seq[n++] = '\r'; break;
        case KEY_BACKSPACE: seq[n++] = 0x7f; break;
        case KEY_TAB: seq[n++] = '\t'; break;
        case KEY_ESC: seq[n++] = 0x1b; break;
        case KEY_UP: memcpy(seq, "\x1b[A", 3); n = 3; break;
        case KEY_DOWN: memcpy(seq, "\x1b[B", 3); n = 3; break;
        case KEY_RIGHT: memcpy(seq, "\x1b[C", 3); n = 3; break;
        case KEY_LEFT: memcpy(seq, "\x1b[D", 3); n = 3; break;
        case KEY_HOME: memcpy(seq, "\x1b[H", 3); n = 3; break;
        case KEY_END: memcpy(seq, "\x1b[F", 3); n = 3; break;
        case KEY_DELETE: memcpy(seq, "\x1b[3~", 4); n = 4; break;
        case KEY_CHAR:
            if (e->ascii)
                seq[n++] = (char)e->ascii;
            break;
        default: break;
        }
    }
    if (n)
        pty_input(T.pty, seq, n);
    wm_invalidate(w);
    return true;
}

static void key(struct window *w, const struct key_event *e)
{
    T.caret = true;
    if (fg_key(w, e))
        return;
    if ((e->mods & MOD_CTRL) && e->key == KEY_CHAR) {
        if (e->ascii == 'l' || e->ascii == 'L') {
            T.count = T.head = 0;
        } else if (e->ascii == 'c' || e->ascii == 'C') {
            char line[VFS_PATH_MAX + CMD_LINE_MAX + 20];
            k_snprintf(line, sizeof line, "aurora:%s$ %s^C", T.cwd, T.input);
            out_color(C_DIM, line);
            T.in_len = T.in_cur = 0;
            T.input[0] = '\0';
        } else if (e->ascii == 'v' || e->ascii == 'V') {
            /* Paste the clipboard's first line; control characters become
             * spaces and the input line limit still applies. */
            size_t n, added = 0;
            const char *clip = clipboard_get(&n);
            for (size_t i = 0; i < n && clip[i] != '\n' && clip[i] != '\r'; i++) {
                if (T.in_len >= CMD_LINE_MAX) {
                    outf(C_ERROR, "paste truncated at the input limit (%d characters)", CMD_LINE_MAX);
                    break;
                }
                char ch = (clip[i] >= 32 && clip[i] < 127) ? clip[i] : ' ';
                memmove(T.input + T.in_cur + 1, T.input + T.in_cur, T.in_len - T.in_cur + 1);
                T.input[T.in_cur++] = ch;
                T.in_len++;
                added++;
            }
            TEST_EVENT("TERM_PASTE added=%zu len=%zu", added, T.in_len);
            (void)added; /* only reported in the test build */
        }
        wm_invalidate(w);
        return;
    }
    switch (e->key) {
    case KEY_ENTER: {
        char line[CMD_LINE_MAX + 1];
        str_copy(line, T.input, sizeof line);
        char echo[VFS_PATH_MAX + CMD_LINE_MAX + 20];
        k_snprintf(echo, sizeof echo, "aurora:%s$ %s", T.cwd, line);
        out_color(C_PROMPT, echo);
        T.in_len = T.in_cur = 0;
        T.input[0] = '\0';
        T.hist_pos = -1;
        T.warned_full = false;
        history_add(line);
        execute(line);
        break;
    }
    case KEY_BACKSPACE:
        if (T.in_cur > 0) {
            memmove(T.input + T.in_cur - 1, T.input + T.in_cur, T.in_len - T.in_cur + 1);
            T.in_cur--;
            T.in_len--;
        }
        break;
    case KEY_DELETE:
        if (T.in_cur < T.in_len) {
            memmove(T.input + T.in_cur, T.input + T.in_cur + 1, T.in_len - T.in_cur);
            T.in_len--;
        }
        break;
    case KEY_LEFT: if (T.in_cur > 0) T.in_cur--; break;
    case KEY_RIGHT: if (T.in_cur < T.in_len) T.in_cur++; break;
    case KEY_HOME: T.in_cur = 0; break;
    case KEY_END: T.in_cur = T.in_len; break;
    case KEY_UP: history_recall(1); break;
    case KEY_DOWN: history_recall(-1); break;
    case KEY_PGUP: T.scroll = MIN(T.scroll + 10, MAX(T.count - 1, 0)); break;
    case KEY_PGDN: T.scroll = MAX(T.scroll - 10, 0); break;
    case KEY_TAB:
    case KEY_CHAR: {
        char ch = e->key == KEY_TAB ? ' ' : (char)e->ascii;
        if (!ch)
            break;
        if (T.in_len >= CMD_LINE_MAX) {
            if (!T.warned_full)
                outf(C_ERROR, "input limit reached (%d characters)", CMD_LINE_MAX);
            T.warned_full = true;
            TEST_EVENT("TERM_LIMIT len=%zu", T.in_len);
            break;
        }
        memmove(T.input + T.in_cur + 1, T.input + T.in_cur, T.in_len - T.in_cur + 1);
        T.input[T.in_cur++] = ch;
        T.in_len++;
        break;
    }
    default:
        break;
    }
    wm_invalidate(w);
}

static void replay_pending(struct window *w)
{
    while (T.pend_off < T.pend_n) {
        char c = T.pending[T.pend_off++];
        if (T.fg_pid && T.pty) {
            pty_input(T.pty, &c, 1); /* the next program gets the rest */
            continue;
        }
        struct key_event e;
        memset(&e, 0, sizeof e);
        e.pressed = true;
        if (c == '\n' || c == '\r') {
            e.key = KEY_ENTER;
        } else if ((unsigned char)c >= 32 && (unsigned char)c < 127) {
            e.key = KEY_CHAR;
            e.ascii = (uint8_t)c;
        } else {
            continue;
        }
        key(w, &e);
    }
}

static void tick(struct window *w, uint64_t now)
{
    if (T.pend_off < T.pend_n)
        replay_pending(w);
    bool on = (now / 500) % 2 == 0;
    if (wm_focused() != w)
        on = false;
    if (on != T.caret) {
        T.caret = on;
        wm_invalidate(w);
    }
}

static void destroyed(struct window *w)
{
    UNUSED(w);
    T.win_id = 0;
    if (T.pty) {
        pty_master_close(T.pty); /* SIGHUP to the programs on it */
        T.pty = NULL;
    }
    T.fg_pid = 0;
}

static void resized(struct window *w)
{
    if (T.pty)
        pty_set_winsize(T.pty, (uint16_t)visible_rows(), (uint16_t)cols());
    wm_invalidate(w);
}

static const struct window_ops ops = {
    .render = render, .key = key, .tick = tick, .destroyed = destroyed, .resized = resized,
};

struct window *terminal_open(void)
{
    struct window *w = wm_get(T.win_id);
    if (w) {
        wm_focus(w);
        return w;
    }
    w = wm_create(APP_TERMINAL, "Terminal", 660, 400, &ops, NULL);
    if (!w)
        return NULL;
    T.win_id = w->id;
    if (!T.cwd[0])
        str_copy(T.cwd, "/home", sizeof T.cwd);
    T.hist_pos = -1;
    T.scroll = 0;
    if (T.count == 0) {
        outf(C_ACCENT, "Aurora Terminal %s", AURORA_VERSION);
        out_color(C_DIM, "Type 'help' for the list of built-in commands.");
    }
    return w;
}
