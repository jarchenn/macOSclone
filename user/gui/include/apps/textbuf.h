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
    size_t anchor;   /* other end of the selection when has_sel */
    bool has_sel;
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

/* Selection: [start, end) between anchor and cursor. Movement functions
 * keep the anchor; callers call tb_select_begin() before a Shift+movement
 * and tb_select_clear() before a plain one. Editing operations replace an
 * active selection. */
void tb_select_begin(struct textbuf *t);
void tb_select_clear(struct textbuf *t);
void tb_select_all(struct textbuf *t);
bool tb_selection(const struct textbuf *t, size_t *start, size_t *end); /* false if empty */
bool tb_delete_selection(struct textbuf *t);
/* Inserts n bytes at the cursor (replacing the selection). All or nothing:
 * returns false and leaves the buffer unchanged if it would not fit. */
bool tb_insert_text(struct textbuf *t, const char *text, size_t n);

/* Desktop-wide plain-text clipboard (single owner, the desktop task). */
#define CLIPBOARD_CAP TEXTBUF_CAP
bool clipboard_set(const char *text, size_t n); /* false if n > cap */
const char *clipboard_get(size_t *n);

#endif
