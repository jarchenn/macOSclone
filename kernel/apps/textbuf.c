#include "apps/textbuf.h"
#include "lib.h"

void tb_init(struct textbuf *t)
{
    t->len = 0;
    t->cursor = 0;
    t->goal_col = 0;
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
