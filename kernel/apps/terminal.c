/* Terminal: a built-in command interpreter (not a POSIX shell). */
#include "desktop/desktop.h"
#include "apps/cmdline.h"
#include "apps/app_util.h"
#include "fs/vfs.h"
#include "core/log.h"
#include "core/time.h"
#include "core/boot.h"
#include "memory/heap.h"
#include "memory/physical.h"
#include "drivers/framebuffer.h"
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
        "  ls [path]            list a directory",
        "  cat <path>           print a file",
        "  echo <text>          print text",
        "  touch <path>         create an empty file",
        "  write <path> <text>  replace a file's content",
        "  mkdir <path>         create a directory",
        "  cd [path] / pwd      change / show the current directory",
        "  open <app|path>      terminal, files, notes, settings, about, or a path",
        "  theme <light|dark>   switch the desktop theme",
        "  reboot               restart the machine (RAM files are lost)",
        "Quote arguments with \"...\"; inside quotes \\n is a newline.",
        "PgUp/PgDn scroll, Up/Down history, Ctrl+C cancels the line.",
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
    out_color(C_DIM, "Apps are built into a single-address-space kernel; they are not");
    out_color(C_DIM, "isolated processes. Files in /home live in RAM and reset on restart.");
}

static void cmd_uptime(void)
{
    uint64_t ms = time_ms();
    uint64_t s = ms / 1000;
    outf(C_TEXT, "up %u:%02u:%02u  (%llu ms, %llu PIT ticks, divisor %u)", (unsigned)(s / 3600),
         (unsigned)(s / 60 % 60), (unsigned)(s % 60), (unsigned long long)ms,
         (unsigned long long)time_ticks(), pit_divisor());
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
    outf(C_TEXT, "RAM files (/home):      %u bytes reserved in %u nodes", u.ram_bytes, u.ram_nodes);
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
                buf[len] = '\0';
                for (size_t i = 0; i < len; i++)
                    if (buf[i] == '\0' || buf[i] == '\r' || buf[i] == '\t')
                        buf[i] = ' ';
                if (len)
                    out_color(C_TEXT, buf);
                TEST_EVENT("CAT path=%s len=%zu crc=%08x", path, len, crc32(buf, len));
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

static void execute(const char *line)
{
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
            outf(C_TEXT, "Aurora %s x86_64 %s-build %s (single-address-space kernel)", AURORA_VERSION, AURORA_VARIANT, AURORA_BUILD_ID);
        else
            out_color(C_TEXT, "Aurora");
    }
    else if (!strcmp(c, "uptime")) cmd_uptime();
    else if (!strcmp(c, "mem")) cmd_mem();
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
    else if (!strcmp(c, "reboot")) {
        out_color(C_DIM, "restarting...");
        desktop_request_reboot();
    }
    else
        outf(C_ERROR, "%s: command not found (try 'help')", c);
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
    k_snprintf(prompt, sizeof prompt, "aurora:%s$ ", T.cwd);
    size_t plen = strlen(prompt);
    size_t total = plen + T.in_len;
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
    str_append(full, T.input, sizeof full);
    size_t caret = plen + T.in_cur;
    bool focused = wm_focused() == w;
    for (int r = 0; r < prompt_rows; r++) {
        size_t off = (size_t)r * (size_t)c;
        size_t n = total > off ? MIN((size_t)c, total - off) : 0;
        int32_t x = 10;
        for (size_t i = 0; i < n; i++) {
            size_t idx = off + i;
            char ch[2] = { full[idx], 0 };
            uint32_t col = idx < 7 ? t->term_prompt : idx < plen ? t->accent : t->term_text;
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

static void key(struct window *w, const struct key_event *e)
{
    T.caret = true;
    if ((e->mods & MOD_CTRL) && e->key == KEY_CHAR) {
        if (e->ascii == 'l' || e->ascii == 'L') {
            T.count = T.head = 0;
        } else if (e->ascii == 'c' || e->ascii == 'C') {
            char line[VFS_PATH_MAX + CMD_LINE_MAX + 20];
            k_snprintf(line, sizeof line, "aurora:%s$ %s^C", T.cwd, T.input);
            out_color(C_DIM, line);
            T.in_len = T.in_cur = 0;
            T.input[0] = '\0';
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

static void tick(struct window *w, uint64_t now)
{
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
}

static void resized(struct window *w) { wm_invalidate(w); }

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
