/* Terminal (0.7: a user process on the toolkit's event loop). Built-in
 * commands plus a pseudo-terminal from /dev/ptmx for programs: they run as
 * session leaders on its slave side, their output goes through a small
 * VT100-subset interpreter (CR, BS, TAB, CSI K/J/H/C/D/G/m) into the
 * scrollback, and while one runs in the foreground every key goes to the
 * pty, whose line discipline echoes and edits (Ctrl-C there is SIGINT for
 * the foreground process group). Text is UTF-8, one cell per code point. */
#include "desktop/desktop.h"
#include "apps/cmdline.h"
#include "apps/app_util.h"
#include "fs/vfs.h"
#include "lib.h"
#include <aurora.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define TERM_LINES 400
#define TERM_COLS 120
#define LINE_BYTES (TERM_COLS * 4)
#define HISTORY 32
#define PAD_X 10
#define PAD_Y 8
#define MAX_CHILDREN 32

enum { C_TEXT = 0, C_DIM, C_ERROR, C_PROMPT, C_ACCENT };

struct term_line {
    char text[LINE_BYTES + 1]; /* UTF-8 */
    uint8_t color;
};

static struct {
    uint32_t win_id;
    struct term_line lines[TERM_LINES];
    int head, count;
    char input[CMD_LINE_MAX * 4 + 1]; /* UTF-8 bytes */
    size_t in_len, in_cur;            /* bytes */
    char history[HISTORY][CMD_LINE_MAX * 4 + 1];
    int hist_count, hist_pos;
    char cwd[VFS_PATH_MAX + 1];
    int scroll;
    bool caret;
    bool warned_full;
    int fg_pid;
    int ptm, pts; /* /dev/ptmx master and its slave, -1 until first use */
    /* Current output line (cells) and the VT100 parser state. */
    uint32_t line[TERM_COLS];
    size_t line_n, line_cur;
    uint8_t line_color, attr;
    int esc;
    int csi[4], ncsi;
    uint8_t partial[4]; /* incomplete UTF-8 sequence from the last read */
    size_t npartial;
    char pending[CMD_LINE_MAX * 2];
    size_t pend_n, pend_off;
    struct { int pid; char name[32]; } kids[MAX_CHILDREN];
} T;

static int32_t cell_w(void) { return text_cell_width(FONT_MONO); }
static int32_t line_h(void) { return text_height(FONT_MONO); }

static int cols(void)
{
    struct window *w = wm_get(T.win_id);
    int c = w ? (w->content.w - 2 * PAD_X) / cell_w() : 80;
    return CLAMP(c, 20, TERM_COLS);
}

static int visible_rows(void)
{
    struct window *w = wm_get(T.win_id);
    return w ? MAX((w->content.h - 12) / line_h(), 1) : 24;
}

static size_t cp_count(const char *s, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        k += ((unsigned char)s[i] & 0xC0) != 0x80;
    return k;
}

/* Bytes of s (n bytes) covering at most `cells` code points. */
static size_t cp_bytes(const char *s, size_t n, size_t cells)
{
    size_t i = 0, c = 0;
    while (i < n && c < cells) {
        i = utf8_next(s, n, i);
        c++;
    }
    return i;
}

static void push_line(const char *s, size_t n, uint8_t color)
{
    int idx = (T.head + T.count) % TERM_LINES;
    if (T.count == TERM_LINES)
        T.head = (T.head + 1) % TERM_LINES;
    else
        T.count++;
    if (n > LINE_BYTES)
        n = LINE_BYTES;
    memcpy(T.lines[idx].text, s, n);
    T.lines[idx].text[n] = '\0';
    T.lines[idx].color = color;
    TEST_EVENT("TERM %s", T.lines[idx].text);
}

static void out_color(uint8_t color, const char *text)
{
    size_t c = (size_t)cols();
    const char *p = text;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len == 0 && nl)
            push_line("", 0, color);
        size_t off = 0;
        while (off < len) {
            size_t n = cp_bytes(p + off, len - off, c);
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
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out_color(color, buf);
}

static int resolve(const char *arg, char *out) { return vfs_normalize(T.cwd, arg, out, VFS_PATH_MAX + 1); }

/* ---- Commands --------------------------------------------------------------- */

static void cmd_help(void)
{
    out_color(C_ACCENT, "Aurora Terminal - built-in commands (not a POSIX shell):");
    static const char *const lines[] = {
        "  help                 this list",
        "  clear                clear the screen (also Ctrl+L)",
        "  about                release and architecture information",
        "  uname [-a]           system identity",
        "  uptime               time since boot",
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
        "  display <w>x<h>      change the screen resolution",
        "  keymap <name>        keyboard layout: us, us-intl (dead keys)",
        "  run <prog> [args] [&] start a user program from /system/bin (& = background)",
        "  <prog> [args]        same as run, for programs in /system/bin",
        "  ps / kill <pid>      list / stop user processes",
        "  ifconfig / netstat   network interface / sockets; see ping, nslookup, fetch",
        "  reboot / poweroff    restart / switch off the machine (ACPI S5)",
        "Quote arguments with \"...\"; inside quotes \\n is a newline.",
        "PgUp/PgDn or the wheel scroll, Up/Down history. Ctrl+C cancels the line or",
        "stops the foreground program; Ctrl+D sends it end-of-input.",
    };
    for (size_t i = 0; i < ARRAY_LEN(lines); i++)
        out_color(C_TEXT, lines[i]);
}

static void cmd_about(void)
{
    struct utsname u;
    uname(&u);
    struct aurora_sysinfo si;
    memset(&si, 0, sizeof si);
    aurora_sysinfo(&si);
    outf(C_ACCENT, "Aurora OS %s (%s)", u.release, u.version);
    outf(C_TEXT, "Architecture: x86_64, %u CPU%s, QEMU 'pc' reference machine", si.cpus, si.cpus == 1 ? "" : "s");
    outf(C_TEXT, "Firmware:     %s", app_firmware_name());
    outf(C_TEXT, "Display:      %dx%d, composited by the display server", desktop_width(), desktop_height());
    out_color(C_DIM, "The desktop and its apps are user processes; the kernel scans the screen out.");
    out_color(C_DIM, app_storage_note());
}

static void cmd_uptime(void)
{
    uint64_t ms = aurora_uptime_ms(), s = ms / 1000;
    outf(C_TEXT, "up %u:%02u:%02u  (%llu ms)", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60),
         (unsigned long long)ms);
}

static void cmd_cpus(void)
{
    static struct aurora_cpus c;
    if (aurora_cpuinfo(&c) != 0) {
        outf(C_ERROR, "cpus: %s", strerror(errno));
        return;
    }
    outf(C_ACCENT, "CPU APIC     TICKS  USER%%  IDLE%%  KERN%%  SWITCHES   IPIS  TLB-IN");
    for (uint32_t i = 0; i < c.n; i++) {
        uint64_t t = c.cpu[i].ticks ? c.cpu[i].ticks : 1;
        outf(c.cpu[i].online ? C_TEXT : C_DIM, "%3u %4u %9llu  %4llu   %4llu   %4llu  %8llu %6llu  %6llu%s",
             c.cpu[i].index, c.cpu[i].apic_id, (unsigned long long)c.cpu[i].ticks,
             (unsigned long long)(c.cpu[i].user_ticks * 100 / t), (unsigned long long)(c.cpu[i].idle_ticks * 100 / t),
             (unsigned long long)(c.cpu[i].kernel_ticks * 100 / t), (unsigned long long)c.cpu[i].switches,
             (unsigned long long)c.cpu[i].ipis, (unsigned long long)c.cpu[i].tlb_shootdowns,
             c.cpu[i].online ? "" : "  (offline)");
        TEST_EVENT("CPU idx=%u apic=%u online=%d ticks=%llu user=%llu idle=%llu switches=%llu tlb=%llu", c.cpu[i].index,
                   c.cpu[i].apic_id, c.cpu[i].online ? 1 : 0, (unsigned long long)c.cpu[i].ticks,
                   (unsigned long long)c.cpu[i].user_ticks, (unsigned long long)c.cpu[i].idle_ticks,
                   (unsigned long long)c.cpu[i].switches, (unsigned long long)c.cpu[i].tlb_shootdowns);
    }
    outf(C_DIM, "%u CPU(s) online; BKL contended %llu times; lockdep %u classes, %u violation(s), %u long hold(s)",
         c.online, (unsigned long long)c.bkl_contended, c.lockdep_classes, c.lockdep_violations, c.lockdep_long_holds);
    TEST_EVENT("CPUS online=%u lockdep_violations=%u long_holds=%u", c.online, c.lockdep_violations,
               c.lockdep_long_holds);
}

static void cmd_mem(void)
{
    struct aurora_sysinfo si;
    struct aurora_klog k;
    memset(&si, 0, sizeof si);
    memset(&k, 0, sizeof k);
    aurora_sysinfo(&si);
    aurora_klog(&k);
    outf(C_TEXT, "Usable RAM detected:    %llu KiB (%llu pages)", (unsigned long long)(k.usable_pages * 4),
         (unsigned long long)k.usable_pages);
    outf(C_TEXT, "Physical pages free:    %llu KiB (%llu pages)", (unsigned long long)(si.free_pages * 4),
         (unsigned long long)si.free_pages);
    outf(C_TEXT, "Kernel heap mapped:     %llu KiB of %llu KiB limit", (unsigned long long)(k.heap_mapped / 1024),
         (unsigned long long)(k.heap_limit / 1024));
    outf(C_TEXT, "Kernel heap in use:     %llu KiB in %llu allocations (peak %llu KiB)",
         (unsigned long long)(k.heap_used / 1024), (unsigned long long)k.heap_allocs,
         (unsigned long long)(k.heap_peak / 1024));
    outf(C_TEXT, "Files (/home):          %u bytes cached in RAM in %u nodes", k.ram_bytes, k.ram_nodes);
    if (k.home_persistent)
        outf(C_TEXT, "Data disk:              %u of %u KiB used, generation %llu, %llu commits", k.pfs_used_blocks * 4,
             k.pfs_total_blocks * 4, (unsigned long long)k.vfs_generation, (unsigned long long)k.pfs_commits);
    else
        outf(C_TEXT, "Data disk:              none (files in /home reset on restart)");
    /* The display server reads /dev/input without a queue of its own, so
     * the only places input can be lost are the kernel's PS/2 rings. */
    outf(C_TEXT, "Input drops:            kbd %u, mouse %u, events %u (%u coalesced)", k.kbd_dropped, k.mouse_dropped,
         0u, 0u);
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
            outf(C_ACCENT, "%s/%*s%s", d.name, (int)(33 - strlen(d.name)), "",
                 d.readonly ? "      dir  read-only" : "      dir");
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
                uint32_t sum = crc32_update(0, buf, len);
                buf[len] = '\0';
                for (size_t i = 0; i < len; i++)
                    if (buf[i] == '\0' || buf[i] == '\r' || buf[i] == '\t')
                        buf[i] = ' ';
                if (len)
                    out_color(C_TEXT, buf);
                TEST_EVENT("CAT path=%s len=%zu crc=%08x", path, len, sum);
                (void)sum;
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
        char text[CMD_LINE_MAX * 4 + 1];
        join_args(2, argc, argv, text, sizeof text);
        r = vfs_write_file(path, text, strlen(text));
        if (r == VFS_OK) {
            outf(C_DIM, "wrote %zu bytes to %s", strlen(text), path);
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
    chdir(T.cwd); /* programs start here */
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
    static const char *const apps[] = { "terminal", "files", "notes", "settings", "welcome" };
    for (size_t i = 0; i < ARRAY_LEN(apps); i++)
        if (ieq(a, apps[i])) {
            if (i == 0) {
                wm_focus(wm_get(T.win_id));
                return;
            }
            ui_launch(apps[i], NULL);
            return;
        }
    if (ieq(a, "about")) {
        ui_launch("settings", "about");
        return;
    }
    char path[VFS_PATH_MAX + 1];
    struct vfs_stat st;
    int r = resolve(a, path);
    if (r == VFS_OK)
        r = vfs_stat(path, &st);
    if (r) {
        outf(C_ERROR, "open: %s: %s", a, vfs_strerror(r));
        return;
    }
    ui_launch(st.type == VFS_DIR ? "files" : "notes", path);
}

static void cmd_theme(int argc, char **argv)
{
    if (argc < 2 || (!ieq(argv[1], "light") && !ieq(argv[1], "dark"))) {
        out_color(C_ERROR, "usage: theme <light|dark>");
        return;
    }
    bool dark = ieq(argv[1], "dark");
    ui_set_theme(dark);
    outf(C_DIM, "theme set to %s", dark ? "dark" : "light");
}

/* ---- Programs ----------------------------------------------------------- */

static void flush_line(void)
{
    if (!T.line_n)
        return;
    char buf[LINE_BYTES + 1];
    size_t n = 0;
    for (size_t i = 0; i < T.line_n && n + 4 <= LINE_BYTES; i++)
        n += utf8_encode(T.line[i], buf + n);
    push_line(buf, n, T.line_color);
    T.line_n = T.line_cur = 0;
    T.line_color = T.attr;
}

static void put_cp(uint32_t cp, size_t c)
{
    if (T.line_cur >= c || T.line_cur >= TERM_COLS)
        flush_line(); /* wrap */
    T.line[T.line_cur++] = cp;
    if (T.line_cur > T.line_n)
        T.line_n = T.line_cur;
    if (T.attr != C_TEXT)
        T.line_color = T.attr;
}

static void csi_final(char f)
{
    int n = T.csi[0];
    switch (f) {
    case 'K':
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

static void vt_byte_cp(uint32_t ch, size_t c)
{
    if (T.esc == 1) {
        if (ch == '[') {
            T.esc = 2;
            T.ncsi = 0;
            memset(T.csi, 0, sizeof T.csi);
        } else {
            T.esc = 0;
        }
        return;
    }
    if (T.esc == 2) {
        if (ch >= '0' && ch <= '9') {
            if (T.ncsi == 0)
                T.ncsi = 1;
            T.csi[T.ncsi - 1] = T.csi[T.ncsi - 1] * 10 + (int)(ch - '0');
        } else if (ch == ';') {
            if (T.ncsi == 0)
                T.ncsi = 1;
            if (T.ncsi < 4)
                T.ncsi++;
        } else if (ch >= 0x40 && ch <= 0x7e) {
            csi_final((char)ch);
            T.esc = 0;
        }
        return;
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
            put_cp(' ', c);
        while (T.line_cur % 8);
        break;
    case '\a': break;
    default:
        put_cp(ch < 32 || ch == 127 ? '?' : ch, c);
        break;
    }
}

/* Program output: UTF-8 decoded across reads, then the VT100 subset. */
static void pty_output(const char *data, size_t len)
{
    size_t c = (size_t)cols();
    size_t i = 0;
    while (i < len) {
        if (T.npartial) {
            T.partial[T.npartial++] = (uint8_t)data[i++];
            size_t used;
            uint32_t cp = utf8_decode((const char *)T.partial, T.npartial, &used);
            if (cp != 0xFFFD || T.npartial == 4 || used < T.npartial) {
                vt_byte_cp(cp, c);
                T.npartial = 0;
            }
            continue;
        }
        unsigned char b = (unsigned char)data[i];
        if (b < 0x80) {
            vt_byte_cp(b, c);
            i++;
            continue;
        }
        size_t need = (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 1;
        if (i + need > len && need > 1) {
            T.npartial = 0;
            while (i < len)
                T.partial[T.npartial++] = (uint8_t)data[i++];
            break;
        }
        size_t used;
        uint32_t cp = utf8_decode(data + i, len - i, &used);
        vt_byte_cp(cp, c);
        i += used;
    }
    T.scroll = 0;
    wm_invalidate(wm_get(T.win_id));
}

static void drain_pty(void)
{
    if (T.ptm < 0)
        return;
    char buf[4096];
    for (int k = 0; k < 16; k++) {
        ssize_t n = read(T.ptm, buf, sizeof buf);
        if (n <= 0)
            break;
        pty_output(buf, (size_t)n);
    }
}

static void pty_readable(void *ctx, int revents)
{
    (void)ctx;
    (void)revents;
    drain_pty();
}

static void set_winsize(void)
{
    if (T.ptm < 0)
        return;
    struct aurora_winsize ws = { (uint16_t)visible_rows(), (uint16_t)cols(), 0, 0 };
    ioctl(T.ptm, AURORA_TIOCSWINSZ, &ws);
}

static bool ensure_pty(void)
{
    if (T.ptm >= 0)
        return true;
    T.ptm = open("/dev/ptmx", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (T.ptm < 0)
        return false;
    T.pts = ioctl(T.ptm, AURORA_TIOCOPENSLAVE, 0);
    if (T.pts < 0) {
        close(T.ptm);
        T.ptm = -1;
        return false;
    }
    fcntl(T.pts, F_SETFD, FD_CLOEXEC); /* children get it through the fd map */
    set_winsize();
    ui_watch_fd(T.ptm, POLLIN, pty_readable, NULL);
    return true;
}

static const char *kid_name(int pid)
{
    for (int i = 0; i < MAX_CHILDREN; i++)
        if (T.kids[i].pid == pid)
            return T.kids[i].name;
    return "?";
}

static void forget_kid(int pid)
{
    for (int i = 0; i < MAX_CHILDREN; i++)
        if (T.kids[i].pid == pid)
            T.kids[i].pid = 0;
}

static void remember_kid(int pid, const char *name)
{
    for (int i = 0; i < MAX_CHILDREN; i++)
        if (!T.kids[i].pid) {
            T.kids[i].pid = pid;
            str_copy(T.kids[i].name, name, sizeof T.kids[i].name);
            return;
        }
}

static void proc_exited(int pid, int status)
{
    drain_pty(); /* its last output comes before the exit line */
    if (pid == T.fg_pid) {
        if (T.ptm >= 0) {
            char tmp[CMD_LINE_MAX * 2];
            struct aurora_buf b = { (uint64_t)(uintptr_t)tmp, sizeof tmp };
            long got = ioctl(T.ptm, AURORA_TIOCTAKEUNREAD, &b);
            size_t n = got > 0 ? (size_t)got : 0;
            /* The unfinished part was echoed onto the current output line;
             * it is shown again as Terminal input, so drop the echo. */
            size_t last = n;
            while (last > 0 && tmp[last - 1] != '\n')
                last--;
            size_t tail = cp_count(tmp + last, n - last);
            if (tail && T.line_n >= tail) {
                bool same = true;
                size_t pos = last;
                for (size_t k = T.line_n - tail; k < T.line_n && same; k++) {
                    size_t used;
                    uint32_t cp = utf8_decode(tmp + pos, n - pos, &used);
                    same = cp == T.line[k];
                    pos += used;
                }
                if (same) {
                    T.line_n -= tail;
                    if (T.line_cur > T.line_n)
                        T.line_cur = T.line_n;
                }
            }
            if (T.pend_off == T.pend_n)
                T.pend_off = T.pend_n = 0;
            for (size_t i = 0; i < n && T.pend_n < sizeof T.pending; i++)
                T.pending[T.pend_n++] = tmp[i];
        }
        flush_line();
        T.fg_pid = 0;
    }
    const char *name = kid_name(pid);
    if (WIFEXITED(status))
        outf(C_DIM, "[%s (pid %d) exited with status %d]", name, pid, WEXITSTATUS(status));
    else
        outf(C_ERROR, "[%s (pid %d) killed by signal %d]", name, pid, WTERMSIG(status));
    forget_kid(pid);
    wm_invalidate(wm_get(T.win_id));
}

static void reap(void)
{
    int st;
    pid_t pid;
    while ((pid = waitpid(-1, &st, WNOHANG)) > 0)
        proc_exited((int)pid, st);
}

static bool find_program(const char *name, char *path)
{
    struct vfs_stat st;
    if (strchr(name, '/'))
        return resolve(name, path) == VFS_OK && vfs_stat(path, &st) == VFS_OK && st.type == VFS_FILE;
    char tmp[VFS_PATH_MAX + 1];
    snprintf(tmp, sizeof tmp, "/system/bin/%s", name);
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
    if (!ensure_pty()) {
        outf(C_ERROR, "run: cannot create a terminal device: %s", strerror(errno));
        return;
    }
    char *av[CMD_MAX_ARGS + 1];
    int n = 0;
    for (int i = first; i < argc && n < CMD_MAX_ARGS; i++)
        av[n++] = argv[i];
    av[n] = NULL;
    char pwd[VFS_PATH_MAX + 8];
    snprintf(pwd, sizeof pwd, "PWD=%s", T.cwd);
    char *envp[] = { "PATH=/system/bin", "HOME=/home", "TERM=vt100", "USER=root", "SHELL=/system/bin/sh", pwd, NULL };
    struct aurora_spawn sa;
    memset(&sa, 0, sizeof sa);
    sa.flags = AURORA_SPAWN_SETSID | AURORA_SPAWN_SETSIGMASK | AURORA_SPAWN_SETSIGDEF |
               (background ? AURORA_SPAWN_BACKGROUND : 0);
    sa.nfds = 3;
    sa.fd_map[0] = sa.fd_map[1] = sa.fd_map[2] = T.pts;
    sa.sigmask = 0;
    sa.sigdefault = ~0ull; /* the Terminal ignores SIGPIPE; programs must not inherit that */
    chdir(T.cwd);
    int pid = aurora_spawn(path, av, envp, &sa);
    if (pid < 0) {
        outf(C_ERROR, "run: %s: %s", argv[first], strerror(errno));
        return;
    }
    const char *base = strrchr(path, '/');
    remember_kid(pid, base ? base + 1 : path);
    if (background) {
        outf(C_DIM, "[%d] %s started in the background", pid, argv[first]);
    } else {
        T.fg_pid = pid;
        T.attr = T.line_color = C_TEXT;
    }
}

static void cmd_ps(void)
{
    static struct aurora_procs l;
    if (aurora_proclist(&l) != 0) {
        outf(C_ERROR, "ps: %s", strerror(errno));
        return;
    }
    outf(C_ACCENT, "  PID  PPID  UID THR STATE    CPU ms  MEM KiB  SYSCALLS  NAME");
    for (uint32_t i = 0; i < l.n; i++)
        outf(C_TEXT, "%5d %5d %4u %3u %-7s %7llu  %7llu  %8llu  %s %s", l.p[i].pid, l.p[i].ppid, l.p[i].uid,
             l.p[i].nthreads, l.p[i].state == 2 ? "zombie" : "running", (unsigned long long)l.p[i].cpu_ms,
             (unsigned long long)l.p[i].mem_kib, (unsigned long long)l.p[i].syscalls, l.p[i].name, l.p[i].args);
    if (!l.n)
        out_color(C_DIM, "  (no user processes)");
    out_color(C_DIM, l.kernel_tasks);
    outf(C_DIM, "context switches: %llu", (unsigned long long)l.context_switches);
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
    if (pid <= 1 || kill(pid, SIGKILL) != 0)
        outf(C_ERROR, "kill: %s: no such process", argv[1]);
}

static bool needs_shell(const char *line)
{
    for (const char *p = line; *p; p++)
        if (strchr("|<>;$*", *p) || (p[0] == '&' && p[1] == '&'))
            return true;
    return false;
}

#ifdef AURORA_TEST
static long parse_num(const char *s)
{
    long v = 0;
    for (; *s >= '0' && *s <= '9'; s++)
        v = v * 10 + (*s - '0');
    return v;
}

/* Test-build only: rewrite /home/stress/f0..f7 for N rounds; every file
 * carries its own CRC so a later check can detect torn or mixed data. */
static void pfsstress(long rounds)
{
    vfs_mkdir("/home/stress");
    static char body[4200];
    for (long rr = 1; rr <= rounds; rr++) {
        for (int k = 0; k < 8; k++) {
            int n = snprintf(body, sizeof body, "round=%ld file=%d ", rr, k);
            int pad = (int)((k * 517 + rr * 131) % 4000);
            for (int i = 0; i < pad; i++)
                body[n++] = (char)('a' + (i + rr) % 26);
            n += snprintf(body + n, sizeof body - (size_t)n, " sum=%08x", crc32_update(0, body, (size_t)n));
            char path[40];
            snprintf(path, sizeof path, "/home/stress/f%d", k);
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

static void pfscheck(void)
{
    static char body[4200];
    int ok = 0, bad = 0, missing = 0;
    long maxround = 0, minround = 1L << 30;
    for (int k = 0; k < 8; k++) {
        char path[40];
        snprintf(path, sizeof path, "/home/stress/f%d", k);
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
        if (good && crc32_update(0, body, len - 13) == want) {
            long rr = parse_num(body + 6);
            maxround = MAX(maxround, rr);
            minround = MIN(minround, rr);
            ok++;
        } else {
            bad++;
        }
    }
    if (!ok)
        minround = 0;
    outf(bad ? C_ERROR : C_TEXT, "pfscheck: ok=%d bad=%d missing=%d rounds=%ld..%ld", ok, bad, missing, minround,
         maxround);
    TEST_EVENT("PFS_CHECK ok=%d bad=%d missing=%d min=%ld max=%ld", ok, bad, missing, minround, maxround);
}
#endif

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
        struct utsname u;
        uname(&u);
        if (cl.argc > 1 && !strcmp(cl.argv[1], "-a"))
            outf(C_TEXT, "Aurora %s x86_64 %s (preemptive, isolated processes)", u.release, u.version);
        else
            out_color(C_TEXT, "Aurora");
    }
    else if (!strcmp(c, "uptime")) cmd_uptime();
    else if (!strcmp(c, "mem")) cmd_mem();
    else if (!strcmp(c, "cpus")) cmd_cpus();
    else if (!strcmp(c, "ls")) cmd_ls(cl.argc, cl.argv);
    else if (!strcmp(c, "cat")) cmd_cat(cl.argc, cl.argv);
    else if (!strcmp(c, "echo")) {
        char text[CMD_LINE_MAX * 4 + 1];
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
    else if (!strcmp(c, "display")) {
        int dw = 0, dh = 0;
        if (cl.argc == 2 && sscanf(cl.argv[1], "%dx%d", &dw, &dh) == 2 && dw > 0 && dh > 0) {
            ui_set_mode(dw, dh);
            outf(C_DIM, "display: asked for %dx%d (see Settings > Display for the list)", dw, dh);
        } else {
            outf(C_TEXT, "display: %dx%d; usage: display <width>x<height>", desktop_width(), desktop_height());
        }
    }
    else if (!strcmp(c, "keymap")) {
        if (cl.argc == 2) {
            ui_set_keymap(cl.argv[1]);
            outf(C_DIM, "keymap: asked for %s (layouts are in /system/keymaps)", cl.argv[1]);
        } else {
            out_color(C_ERROR, "usage: keymap <name>   (e.g. us, us-intl)");
        }
    }
#ifdef AURORA_TEST
    else if (!strcmp(c, "tlbtest")) {
        bool flush = !(cl.argc == 2 && !strcmp(cl.argv[1], "noflush"));
        int32_t v[6] = { 0 };
        long tr = aurora_testop(AURORA_TESTOP_TLB, flush, v);
        int tres = tr < 0 ? -errno : v[0];
        if (tres)
            outf(C_ERROR, "tlbtest: could not run (%d)", tres);
        else
            outf(C_TEXT, "tlbtest: %s; cpu %d unmapped, cpu %d read before=%s after=%s",
                 flush ? "shootdown" : "no shootdown", v[1], v[2], v[3] ? "ok" : "BAD",
                 v[4] ? "fault" : (v[5] ? "stale value" : "other"));
        TEST_EVENT("TLBTEST flush=%d r=%d unmap_cpu=%d reader_cpu=%d before=%d faulted=%d stale=%d", flush ? 1 : 0,
                   tres, v[1], v[2], v[3], v[4], v[5]);
    }
    else if (!strcmp(c, "pfsstress") && cl.argc == 2) pfsstress(parse_num(cl.argv[1]));
    else if (!strcmp(c, "pfscheck")) pfscheck();
    else if (!strcmp(c, "testfill") && cl.argc == 3) {
        static char buf[RAMFS_MAX_FILE + 2];
        uint32_t n = (uint32_t)parse_num(cl.argv[2]);
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
        if (aurora_power(2) != 0)
            outf(C_ERROR, "poweroff: %s", strerror(errno));
    }
    else if (!strcmp(c, "reboot")) {
        out_color(C_DIM, "restarting...");
        if (aurora_power(1) != 0)
            outf(C_ERROR, "reboot: %s", strerror(errno));
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
                str_append(to, base, sizeof to);
            }
            if (er == VFS_OK)
                er = vfs_rename(from, to);
            if (er)
                outf(C_ERROR, "mv: %s", vfs_strerror(er));
        }
    }
    else if (!strcmp(c, "sync")) {
        vfs_sync();
        out_color(C_DIM, vfs_home_persistent() ? "sync: all changes are on the data disk"
                                               : "sync: no data disk (nothing to flush)");
    }
    else if (!strcmp(c, "df")) {
        struct aurora_klog k;
        memset(&k, 0, sizeof k);
        aurora_klog(&k);
        if (k.home_persistent)
            outf(C_TEXT, "/home  data disk  %u KiB used of %u KiB  (generation %llu, %u corrupt files skipped)",
                 k.pfs_used_blocks * 4, k.pfs_total_blocks * 4, (unsigned long long)k.vfs_generation,
                 k.pfs_corrupt_files);
        else
            outf(C_TEXT, "/home  RAM        %u KiB used of %u KiB (lost on restart)", k.ram_bytes / 1024,
                 RAMFS_MAX_TOTAL / 1024);
        out_color(C_TEXT, "/system  initramfs  read-only");
    }
    else if (!strcmp(c, "ifconfig")) {
        struct aurora_netinfo ni;
        memset(&ni, 0, sizeof ni);
        if (aurora_netinfo(&ni) != 0 || !ni.up) {
            out_color(C_TEXT, "net0: no network card (start QEMU with a virtio-net device)");
        } else {
            char ip[16], mask[16], gw[16], dns[16];
            aurora_format_ipv4(ni.ip, ip, sizeof ip);
            aurora_format_ipv4(ni.netmask, mask, sizeof mask);
            aurora_format_ipv4(ni.gateway, gw, sizeof gw);
            aurora_format_ipv4(ni.dns, dns, sizeof dns);
            outf(C_TEXT, "net0: virtio-net  mac %02x:%02x:%02x:%02x:%02x:%02x  %s", ni.mac[0], ni.mac[1], ni.mac[2],
                 ni.mac[3], ni.mac[4], ni.mac[5], ni.configured ? "configured (DHCP)" : "not configured");
            if (ni.configured)
                outf(C_TEXT, "      inet %s  netmask %s  gateway %s  dns %s", ip, mask, gw, dns);
            outf(C_TEXT, "      rx %llu packets  tx %llu packets", (unsigned long long)ni.rx_packets,
                 (unsigned long long)ni.tx_packets);
        }
    }
    else if (!strcmp(c, "netstat")) {
        struct aurora_klog k;
        memset(&k, 0, sizeof k);
        aurora_klog(&k);
        outf(C_TEXT, "%u socket(s) open", k.sockets);
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

static bool fg_running(void) { return T.fg_pid > 0; }

static void render(struct window *w, struct surface *s)
{
    const struct theme *t = g_theme;
    int32_t cw = cell_w(), lh = line_h();
    draw_fill(s, rect_make(0, 0, s->w, s->h), t->term_bg);
    int rows = MAX((s->h - 12) / lh, 1);
    size_t c = (size_t)cols();
    /* Bottom: the prompt and input (or the foreground program's line). */
    uint32_t full[TERM_COLS * 4 + VFS_PATH_MAX + 16];
    size_t total = 0, plen = 0, caret = 0;
    bool fg = fg_running();
    if (fg) {
        for (size_t i = 0; i < T.line_n; i++)
            full[total++] = T.line[i];
        caret = MIN(T.line_cur, total);
        plen = total;
    } else {
        char prompt[VFS_PATH_MAX + 16];
        snprintf(prompt, sizeof prompt, "aurora:%s$ ", T.cwd);
        for (size_t i = 0; prompt[i];) {
            size_t used;
            full[total++] = utf8_decode(prompt + i, strlen(prompt + i), &used);
            i += used;
        }
        plen = total;
        for (size_t i = 0; i < T.in_len && total < ARRAY_LEN(full);) {
            if (i == T.in_cur)
                caret = total;
            size_t used;
            full[total++] = utf8_decode(T.input + i, T.in_len - i, &used);
            i += used;
        }
        if (T.in_cur >= T.in_len)
            caret = total;
    }
    int prompt_rows = (int)(total / c) + 1;
    int out_rows = rows - prompt_rows;
    int last = T.count - T.scroll;
    int first = last - out_rows;
    if (first < 0)
        first = 0;
    int32_t y = PAD_Y;
    for (int i = first; i < last; i++) {
        const struct term_line *ln = &T.lines[(T.head + i) % TERM_LINES];
        text_draw(s, FONT_MONO, PAD_X, y, ln->text, color_of(ln->color));
        y += lh;
    }
    if (T.scroll > 0) {
        const char *m = "-- scrolled back --";
        text_draw(s, FONT_MONO, s->w - PAD_X - text_width(FONT_MONO, m), PAD_Y, m, t->text_dim);
        return;
    }
    for (int r = 0; r < prompt_rows; r++) {
        size_t off = (size_t)r * c;
        size_t n = total > off ? MIN(c, total - off) : 0;
        for (size_t i = 0; i < n; i++) {
            size_t idx = off + i;
            uint32_t col = fg ? color_of(T.line_color) : idx < 7 ? t->term_prompt : idx < plen ? t->accent
                                                                                              : t->term_text;
            text_draw_cp(s, FONT_MONO, PAD_X + (int32_t)i * cw, y, full[idx], col);
        }
        if (w->focused && caret >= off && caret < off + c && T.caret) {
            int32_t cx = PAD_X + (int32_t)(caret - off) * cw;
            draw_fill(s, rect_make(cx, y, cw, lh), t->term_prompt);
            if (caret < total)
                text_draw_cp(s, FONT_MONO, cx, y, full[caret], t->term_bg);
        }
        y += lh;
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

static void pty_write(const char *s, size_t n)
{
    if (T.ptm >= 0 && n)
        write(T.ptm, s, n);
}

/* Insert UTF-8 text at the cursor (the 200-character line limit counts
 * code points). Returns the characters added. */
static size_t input_insert(const char *s, size_t n)
{
    size_t added = 0;
    for (size_t i = 0; i < n;) {
        size_t used;
        uint32_t cp = utf8_decode(s + i, n - i, &used);
        if (cp < 32 || cp == 127)
            cp = ' ';
        char enc[4];
        size_t el = utf8_encode(cp, enc);
        if (cp_count(T.input, T.in_len) >= CMD_LINE_MAX) {
            if (!T.warned_full)
                outf(C_ERROR, "input limit reached (%d characters)", CMD_LINE_MAX);
            T.warned_full = true;
            TEST_EVENT("TERM_LIMIT len=%zu", cp_count(T.input, T.in_len));
            break;
        }
        if (T.in_len + el >= sizeof T.input)
            break;
        memmove(T.input + T.in_cur + el, T.input + T.in_cur, T.in_len - T.in_cur + 1);
        memcpy(T.input + T.in_cur, enc, el);
        T.in_cur += el;
        T.in_len += el;
        added++;
        i += used;
    }
    return added;
}

static void paste_cb(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    size_t k = 0;
    while (k < len && data[k] != '\n' && data[k] != '\r')
        k++;
    if (fg_running()) {
        pty_write(data, k);
        TEST_EVENT("TERM_PASTE added=%zu len=%zu", cp_count(data, k), k);
    } else {
        size_t before = cp_count(T.input, T.in_len);
        size_t added = input_insert(data, k);
        if (added < cp_count(data, k))
            outf(C_ERROR, "paste truncated at the input limit (%d characters)", CMD_LINE_MAX);
        TEST_EVENT("TERM_PASTE added=%zu len=%zu", added, before + added);
    }
    wm_invalidate(wm_get(T.win_id));
}

static bool fg_key(struct window *w, const struct key_event *e)
{
    if (!fg_running())
        return false;
    if (e->key == KEY_PGUP || e->key == KEY_PGDN)
        return false; /* scrollback stays with the Terminal */
    char seq[8];
    size_t n = 0;
    if ((e->mods & MOD_CTRL) && e->key == KEY_CHAR && (e->ascii | 0x20) == 'v') {
        ui_clipboard_get(paste_cb, NULL);
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
        default: break;
        }
    }
    pty_write(seq, n);
    wm_invalidate(w);
    return true;
}

static void submit(void)
{
    char line[sizeof T.input];
    str_copy(line, T.input, sizeof line);
    char echo[VFS_PATH_MAX + sizeof T.input + 20];
    snprintf(echo, sizeof echo, "aurora:%s$ %s", T.cwd, line);
    out_color(C_PROMPT, echo);
    T.in_len = T.in_cur = 0;
    T.input[0] = '\0';
    T.hist_pos = -1;
    T.warned_full = false;
    history_add(line);
    execute(line);
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
            char line[VFS_PATH_MAX + sizeof T.input + 20];
            snprintf(line, sizeof line, "aurora:%s$ %s^C", T.cwd, T.input);
            out_color(C_DIM, line);
            T.in_len = T.in_cur = 0;
            T.input[0] = '\0';
        } else if (e->ascii == 'v' || e->ascii == 'V') {
            ui_clipboard_get(paste_cb, NULL);
        }
        wm_invalidate(w);
        return;
    }
    switch (e->key) {
    case KEY_ENTER: submit(); break;
    case KEY_BACKSPACE:
        if (T.in_cur > 0) {
            size_t p = utf8_prev(T.input, T.in_cur);
            memmove(T.input + p, T.input + T.in_cur, T.in_len - T.in_cur + 1);
            T.in_len -= T.in_cur - p;
            T.in_cur = p;
        }
        break;
    case KEY_DELETE:
        if (T.in_cur < T.in_len) {
            size_t nx = utf8_next(T.input, T.in_len, T.in_cur);
            memmove(T.input + T.in_cur, T.input + nx, T.in_len - nx + 1);
            T.in_len -= nx - T.in_cur;
        }
        break;
    case KEY_LEFT: T.in_cur = utf8_prev(T.input, T.in_cur); break;
    case KEY_RIGHT: T.in_cur = utf8_next(T.input, T.in_len, T.in_cur); break;
    case KEY_HOME: T.in_cur = 0; break;
    case KEY_END: T.in_cur = T.in_len; break;
    case KEY_UP: history_recall(1); break;
    case KEY_DOWN: history_recall(-1); break;
    case KEY_PGUP: T.scroll = MIN(T.scroll + 10, MAX(T.count - 1, 0)); break;
    case KEY_PGDN: T.scroll = MAX(T.scroll - 10, 0); break;
    case KEY_TAB: input_insert(" ", 1); break;
    default: break;
    }
    wm_invalidate(w);
}

static void text(struct window *w, const char *utf8)
{
    T.caret = true;
    if (fg_running()) {
        pty_write(utf8, strlen(utf8));
        wm_invalidate(w);
        return;
    }
    input_insert(utf8, strlen(utf8));
    wm_invalidate(w);
}

static void mouse(struct window *w, const struct win_mouse *m)
{
    if (m->kind == WM_MOUSE_WHEEL) {
        T.scroll = CLAMP(T.scroll + m->wheel * 3, 0, MAX(T.count - 1, 0));
        TEST_EVENT("TERM_SCROLL wheel=%d scroll=%d", m->wheel, T.scroll);
        wm_invalidate(w);
    }
}

static void replay_pending(struct window *w)
{
    while (T.pend_off < T.pend_n) {
        char c = T.pending[T.pend_off++];
        if (fg_running()) {
            pty_write(&c, 1); /* the next program gets the rest */
            continue;
        }
        if (c == '\n' || c == '\r') {
            struct key_event e = { KEY_ENTER, 0, 0, true };
            key(w, &e);
        } else if ((unsigned char)c >= 32) {
            char s[2] = { c, 0 };
            input_insert(s, 1);
        }
    }
    wm_invalidate(w);
}

static void idle(void)
{
    reap();
    struct window *w = wm_get(T.win_id);
    if (w && T.pend_off < T.pend_n)
        replay_pending(w);
}

static void tick(struct window *w, uint64_t now)
{
    bool on = (now / 500) % 2 == 0 && w->focused;
    if (on != T.caret) {
        T.caret = on;
        wm_invalidate(w);
    }
}

static void destroyed(struct window *w)
{
    (void)w;
    T.win_id = 0;
    if (T.ptm >= 0) {
        ui_unwatch_fd(T.ptm);
        close(T.ptm); /* hang-up: SIGHUP to the programs on it */
        close(T.pts);
        T.ptm = T.pts = -1;
    }
    T.fg_pid = 0;
}

static void resized(struct window *w)
{
    set_winsize();
    wm_invalidate(w);
}

static const struct window_ops ops = {
    .render = render, .key = key, .text = text, .mouse = mouse, .tick = tick, .destroyed = destroyed,
    .resized = resized,
};

static void on_sigchld(int s) { (void)s; } /* interrupts poll(); idle() reaps */

static struct window *terminal_open(const char *arg)
{
    (void)arg;
    struct window *w = wm_get(T.win_id);
    if (w) {
        wm_focus(w);
        return w;
    }
    w = wm_create_ex(APP_TERMINAL, "Terminal", 660, 400, WINF_RESIZABLE, 240, 120, 0, 0, &ops, NULL);
    if (!w)
        return NULL;
    T.win_id = w->id;
    if (!T.cwd[0])
        str_copy(T.cwd, "/home", sizeof T.cwd);
    chdir(T.cwd);
    T.hist_pos = -1;
    T.scroll = 0;
    if (T.count == 0) {
        outf(C_ACCENT, "Aurora Terminal %s", AURORA_VERSION);
        out_color(C_DIM, "Type 'help' for the list of built-in commands.");
    }
    return w;
}

int main(int argc, char **argv)
{
    T.ptm = T.pts = -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigaction(SIGCHLD, &sa, NULL);
    ui_set_idle(idle);
    return app_main(APP_TERMINAL, "Terminal", terminal_open, argc, argv);
}
