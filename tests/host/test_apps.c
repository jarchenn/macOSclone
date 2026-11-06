/* Terminal command parsing, Notes text buffer, formatter. */
#include "harness.h"
#include "apps/cmdline.h"
#include "apps/textbuf.h"
#include "lib.h"

const char *t_suite = "apps";

TEST(cmdline_basic)
{
    struct cmdline c;
    CHECK_EQ(cmdline_parse("  ls   /home  ", &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 2);
    CHECK_STR(c.argv[0], "ls");
    CHECK_STR(c.argv[1], "/home");
    CHECK_EQ(cmdline_parse("", &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 0);
    CHECK_EQ(cmdline_parse("     ", &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 0);
}

TEST(cmdline_quotes_and_escapes)
{
    struct cmdline c;
    CHECK_EQ(cmdline_parse("write a.txt \"hello  world\"", &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 3);
    CHECK_STR(c.argv[2], "hello  world");
    CHECK_EQ(cmdline_parse("echo \"say \\\"hi\\\"\" x\\ y", &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 3);
    CHECK_STR(c.argv[1], "say \"hi\"");
    CHECK_STR(c.argv[2], "x y");
    CHECK_EQ(cmdline_parse("write f \"a\\nb\"", &c), CMDLINE_OK);
    CHECK_STR(c.argv[2], "a\nb");
    CHECK_EQ(cmdline_parse("echo \"\"", &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 2);
    CHECK_STR(c.argv[1], "");
    CHECK_EQ(cmdline_parse("echo ab\"cd ef\"gh", &c), CMDLINE_OK);
    CHECK_STR(c.argv[1], "abcd efgh");
}

TEST(cmdline_limits)
{
    struct cmdline c;
    char line[CMD_LINE_MAX + 2];
    memset(line, 'a', CMD_LINE_MAX);
    line[CMD_LINE_MAX] = 0;
    CHECK_EQ(cmdline_parse(line, &c), CMDLINE_OK);
    CHECK_EQ(strlen(c.argv[0]), CMD_LINE_MAX);
    line[CMD_LINE_MAX] = 'a';
    line[CMD_LINE_MAX + 1] = 0;
    CHECK_EQ(cmdline_parse(line, &c), CMDLINE_TOO_LONG);
    CHECK_EQ(cmdline_parse("a b c d e f g h i j k l m n o p", &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 16);
    CHECK_EQ(cmdline_parse("a b c d e f g h i j k l m n o p q", &c), CMDLINE_TOO_MANY);
    CHECK_EQ(cmdline_parse("echo \"unterminated", &c), CMDLINE_UNTERMINATED);
    /* worst case storage: max args each 1 char + terminators */
    char many[CMD_LINE_MAX + 1] = "";
    for (int i = 0; i < 16; i++)
        strcat(many, "\"\" ");
    CHECK_EQ(cmdline_parse(many, &c), CMDLINE_OK);
    CHECK_EQ(c.argc, 16);
}

static struct textbuf tb;

TEST(textbuf_editing)
{
    tb_init(&tb);
    CHECK(!tb_backspace(&tb)); /* nothing to delete at start */
    CHECK(!tb_delete(&tb));
    const char *s = "ab\ncd";
    for (const char *p = s; *p; p++)
        CHECK(tb_insert(&tb, *p));
    CHECK_EQ(tb.len, 5);
    CHECK(memcmp(tb.data, "ab\ncd", 5) == 0);
    tb_up(&tb);
    CHECK_EQ(tb.cursor, 2); /* end of "ab" (goal col 2) */
    tb_home(&tb);
    CHECK_EQ(tb.cursor, 0);
    tb_left(&tb);
    CHECK_EQ(tb.cursor, 0);
    tb_end(&tb);
    tb_right(&tb);
    CHECK_EQ(tb.cursor, 3);
    CHECK(tb_backspace(&tb)); /* joins lines */
    CHECK(memcmp(tb.data, "abcd", 4) == 0);
    CHECK_EQ(tb.cursor, 2);
    CHECK(tb_delete(&tb));
    CHECK(memcmp(tb.data, "abd", 3) == 0);
    tb_end(&tb);
    CHECK(!tb_delete(&tb)); /* at end */
    tb_down(&tb);
    CHECK_EQ(tb.cursor, tb.len);
    CHECK_EQ(tb_line_number(&tb, tb.len), 0);
}

TEST(textbuf_capacity)
{
    tb_init(&tb);
    for (int i = 0; i < TEXTBUF_CAP; i++)
        if (!tb_insert(&tb, 'x')) {
            CHECK(false);
            break;
        }
    CHECK_EQ(tb.len, TEXTBUF_CAP);
    CHECK(!tb_insert(&tb, 'y')); /* full: refused, not corrupted */
    CHECK_EQ(tb.len, TEXTBUF_CAP);
    tb_home(&tb);
    CHECK(!tb_insert(&tb, '\n'));
    CHECK_EQ(tb.data[0], 'x');
    static char big[TEXTBUF_CAP + 1];
    CHECK(!tb_set(&tb, big, TEXTBUF_CAP + 1));
    CHECK(tb_set(&tb, "hi", 2));
    CHECK_EQ(tb.len, 2);
    CHECK_EQ(tb.cursor, 0);
}

TEST(textbuf_selection_clipboard)
{
    size_t a, b, n;
    tb_init(&tb);
    CHECK(tb_insert_text(&tb, "hello world", 11));
    CHECK(!tb_selection(&tb, &a, &b));
    tb_home(&tb);
    tb_select_begin(&tb);
    for (int i = 0; i < 5; i++)
        tb_right(&tb);
    CHECK(tb_selection(&tb, &a, &b));
    CHECK_EQ(a, 0);
    CHECK_EQ(b, 5);
    CHECK(clipboard_set(tb.data + a, b - a));
    CHECK(tb_insert(&tb, 'J')); /* typing replaces the selection */
    CHECK_EQ(tb.len, 7);
    CHECK(memcmp(tb.data, "J world", 7) == 0);
    tb_end(&tb);
    const char *c = clipboard_get(&n);
    CHECK_EQ(n, 5);
    CHECK(tb_insert_text(&tb, c, n));
    CHECK(memcmp(tb.data, "J worldhello", 12) == 0);
    /* Backwards selection, then backspace deletes it. */
    tb_select_begin(&tb);
    tb_left(&tb);
    tb_left(&tb);
    CHECK(tb_selection(&tb, &a, &b));
    CHECK_EQ(a, 10);
    CHECK_EQ(b, 12);
    CHECK(tb_backspace(&tb));
    CHECK_EQ(tb.len, 10);
    CHECK_EQ(tb.cursor, 10);
    CHECK(!tb_selection(&tb, &a, &b));
    tb_select_all(&tb);
    CHECK(tb_selection(&tb, &a, &b));
    CHECK_EQ(b - a, 10);
    CHECK(tb_delete(&tb));
    CHECK_EQ(tb.len, 0);
    /* A paste that does not fit is refused without changes. */
    static char big[TEXTBUF_CAP];
    memset(big, 'z', sizeof big);
    CHECK(tb_insert_text(&tb, "ab", 2));
    CHECK(!tb_insert_text(&tb, big, TEXTBUF_CAP - 1));
    CHECK_EQ(tb.len, 2);
    tb_select_all(&tb); /* replacing the whole buffer fits exactly */
    CHECK(tb_insert_text(&tb, big, TEXTBUF_CAP));
    CHECK_EQ(tb.len, TEXTBUF_CAP);
    CHECK(clipboard_set(big, CLIPBOARD_CAP));
    CHECK(!clipboard_set(big, CLIPBOARD_CAP + 1));
    clipboard_get(&n);
    CHECK_EQ(n, CLIPBOARD_CAP);
}

TEST(format)
{
    char b[64];
    CHECK_EQ(k_snprintf(b, sizeof b, "%d|%5d|%-4d|%05d", -42, 7, 3, -12), 20);
    CHECK_STR(b, "-42|    7|3   |-0012");
    k_snprintf(b, sizeof b, "%x %X %08llx %llu", 255u, 255u, (unsigned long long)0xabcULL, (unsigned long long)18446744073709551615ULL);
    CHECK_STR(b, "ff FF 00000abc 18446744073709551615");
    k_snprintf(b, sizeof b, "[%s][%.3s][%6s][%-6s]", "abc", "abcdef", "ab", "ab");
    CHECK_STR(b, "[abc][abc][    ab][ab    ]");
    k_snprintf(b, sizeof b, "%c%%%s", 'Z', (const char *)NULL);
    CHECK_STR(b, "Z%(null)");
    char small[6];
    int n = k_snprintf(small, sizeof small, "hello world");
    CHECK_EQ(n, 11); /* would-be length */
    CHECK_STR(small, "hello");
    k_snprintf(b, sizeof b, "%*d|%zu", 4, 9, (size_t)123);
    CHECK_STR(b, "   9|123");
    CHECK_EQ(crc32("123456789", 9), 0xCBF43926u); /* standard check value */
}

void run_tests(void)
{
    RUN(cmdline_basic);
    RUN(cmdline_quotes_and_escapes);
    RUN(cmdline_limits);
    RUN(textbuf_editing);
    RUN(textbuf_capacity);
    RUN(textbuf_selection_clipboard);
    RUN(format);
}
