#include "apps/textbuf.h"
#include "lib.h"

void tb_init(struct textbuf *t)
{
    t->len = 0;
    t->cursor = 0;
    t->goal_col = 0;
    t->anchor = 0;
    t->has_sel = false;
    t->data[0] = '\0';
}

bool tb_set(struct textbuf *t, const char *text, size_t len)
{
    if (len > TEXTBUF_CAP)
        return false;
    memcpy(t->data, text, len);
    t->len = len;
    t->cursor = 0;
    t->goal_col = 0;
    t->has_sel = false;
    return true;
}

size_t tb_line_start(const struct textbuf *t, size_t pos)
{
    while (pos > 0 && t->data[pos - 1] != '\n')
        pos--;
    return pos;
}

size_t tb_line_end(const struct textbuf *t, size_t pos)
{
    while (pos < t->len && t->data[pos] != '\n')
        pos++;
    return pos;
}

size_t tb_line_number(const struct textbuf *t, size_t pos)
{
    size_t n = 0;
    for (size_t i = 0; i < pos && i < t->len; i++)
        if (t->data[i] == '\n')
            n++;
    return n;
}

static void update_goal(struct textbuf *t) { t->goal_col = t->cursor - tb_line_start(t, t->cursor); }

bool tb_insert(struct textbuf *t, char c)
{
    tb_delete_selection(t);
    if (t->len >= TEXTBUF_CAP)
        return false;
    memmove(t->data + t->cursor + 1, t->data + t->cursor, t->len - t->cursor);
    t->data[t->cursor++] = c;
    t->len++;
    update_goal(t);
    return true;
}

bool tb_backspace(struct textbuf *t)
{
    if (tb_delete_selection(t))
        return true;
    if (t->cursor == 0)
        return false;
    memmove(t->data + t->cursor - 1, t->data + t->cursor, t->len - t->cursor);
    t->cursor--;
    t->len--;
    update_goal(t);
    return true;
}

bool tb_delete(struct textbuf *t)
{
    if (tb_delete_selection(t))
        return true;
    if (t->cursor >= t->len)
        return false;
    memmove(t->data + t->cursor, t->data + t->cursor + 1, t->len - t->cursor - 1);
    t->len--;
    return true;
}

void tb_left(struct textbuf *t)
{
    if (t->cursor > 0)
        t->cursor--;
    update_goal(t);
}

void tb_right(struct textbuf *t)
{
    if (t->cursor < t->len)
        t->cursor++;
    update_goal(t);
}

void tb_home(struct textbuf *t)
{
    t->cursor = tb_line_start(t, t->cursor);
    update_goal(t);
}

void tb_end(struct textbuf *t)
{
    t->cursor = tb_line_end(t, t->cursor);
    update_goal(t);
}

static void move_to_col(struct textbuf *t, size_t line_start)
{
    size_t end = tb_line_end(t, line_start);
    size_t pos = line_start + t->goal_col;
    t->cursor = pos > end ? end : pos;
}

void tb_up(struct textbuf *t)
{
    size_t start = tb_line_start(t, t->cursor);
    if (start == 0) {
        t->cursor = 0;
        return;
    }
    move_to_col(t, tb_line_start(t, start - 1));
}

void tb_down(struct textbuf *t)
{
    size_t end = tb_line_end(t, t->cursor);
    if (end >= t->len) {
        t->cursor = t->len;
        return;
    }
    move_to_col(t, end + 1);
}

void tb_select_begin(struct textbuf *t)
{
    if (!t->has_sel) {
        t->anchor = t->cursor;
        t->has_sel = true;
    }
}

void tb_select_clear(struct textbuf *t) { t->has_sel = false; }

void tb_select_all(struct textbuf *t)
{
    t->anchor = 0;
    t->cursor = t->len;
    t->has_sel = true;
    update_goal(t);
}

bool tb_selection(const struct textbuf *t, size_t *start, size_t *end)
{
    if (!t->has_sel || t->anchor == t->cursor || t->anchor > t->len)
        return false;
    *start = t->anchor < t->cursor ? t->anchor : t->cursor;
    *end = t->anchor < t->cursor ? t->cursor : t->anchor;
    return true;
}

bool tb_delete_selection(struct textbuf *t)
{
    size_t a, b;
    bool any = tb_selection(t, &a, &b);
    t->has_sel = false;
    if (!any)
        return false;
    memmove(t->data + a, t->data + b, t->len - b);
    t->len -= b - a;
    t->cursor = a;
    update_goal(t);
    return true;
}

bool tb_insert_text(struct textbuf *t, const char *text, size_t n)
{
    size_t a = t->cursor, b = t->cursor;
    tb_selection(t, &a, &b);
    if (t->len - (b - a) + n > TEXTBUF_CAP)
        return false;
    tb_delete_selection(t);
    memmove(t->data + t->cursor + n, t->data + t->cursor, t->len - t->cursor);
    memcpy(t->data + t->cursor, text, n);
    t->cursor += n;
    t->len += n;
    update_goal(t);
    return true;
}

static char clip_data[CLIPBOARD_CAP];
static size_t clip_len;

bool clipboard_set(const char *text, size_t n)
{
    if (n > CLIPBOARD_CAP)
        return false;
    memcpy(clip_data, text, n);
    clip_len = n;
    return true;
}

const char *clipboard_get(size_t *n)
{
    *n = clip_len;
    return clip_data;
}
