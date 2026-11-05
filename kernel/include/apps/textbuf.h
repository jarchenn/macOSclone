#ifndef AURORA_TEXTBUF_H
#define AURORA_TEXTBUF_H
#include "types.h"

/* Bounded plain-text edit buffer used by Notes. Never writes past
 * TEXTBUF_CAP; operations that would overflow return false. */
#define TEXTBUF_CAP 16384

struct textbuf {
    char data[TEXTBUF_CAP];
    size_t len;
    size_t cursor;
    size_t goal_col; /* preferred column for vertical movement */
};

void tb_init(struct textbuf *t);
bool tb_set(struct textbuf *t, const char *text, size_t len);
bool tb_insert(struct textbuf *t, char c);
bool tb_backspace(struct textbuf *t);
bool tb_delete(struct textbuf *t);
void tb_left(struct textbuf *t);
void tb_right(struct textbuf *t);
void tb_home(struct textbuf *t);
void tb_end(struct textbuf *t);
void tb_up(struct textbuf *t);
void tb_down(struct textbuf *t);
size_t tb_line_start(const struct textbuf *t, size_t pos);
size_t tb_line_end(const struct textbuf *t, size_t pos);
size_t tb_line_number(const struct textbuf *t, size_t pos);

#endif
