#ifndef AURORA_CMDLINE_H
#define AURORA_CMDLINE_H
#include "types.h"

/* Terminal command-line tokenizer.
 *  - Arguments are separated by spaces.
 *  - "double quotes" group words; inside quotes \" \\ and \n are escapes.
 *  - Outside quotes, a backslash makes the next character literal.
 *  - At most CMD_MAX_ARGS arguments and CMD_LINE_MAX input bytes.
 * Not a POSIX shell: no pipes, redirection, globbing or variables. */
#define CMD_LINE_MAX 200
#define CMD_MAX_ARGS 16

enum cmdline_err { CMDLINE_OK = 0, CMDLINE_TOO_LONG = -1, CMDLINE_TOO_MANY = -2, CMDLINE_UNTERMINATED = -3 };

struct cmdline {
    int argc;
    char *argv[CMD_MAX_ARGS];
    char storage[CMD_LINE_MAX + CMD_MAX_ARGS + 1];
};

int cmdline_parse(const char *line, struct cmdline *out);
const char *cmdline_strerror(int err);

#endif
