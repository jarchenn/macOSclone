#include "apps/cmdline.h"
#include "lib.h"

const char *cmdline_strerror(int err)
{
    switch (err) {
    case CMDLINE_TOO_LONG: return "command line too long (200 characters max)";
    case CMDLINE_TOO_MANY: return "too many arguments (16 max)";
    case CMDLINE_UNTERMINATED: return "unterminated quote";
    default: return "ok";
    }
}

int cmdline_parse(const char *line, struct cmdline *out)
{
    out->argc = 0;
    if (strlen(line) > CMD_LINE_MAX)
        return CMDLINE_TOO_LONG;
    size_t w = 0;
    const char *p = line;
    for (;;) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        if (out->argc >= CMD_MAX_ARGS)
            return CMDLINE_TOO_MANY;
        out->argv[out->argc++] = &out->storage[w];
        bool quoted = false;
        while (*p) {
            char c = *p;
            if (quoted) {
                if (c == '"') {
                    quoted = false;
                    p++;
                    continue;
                }
                if (c == '\\' && p[1]) {
                    char e = p[1];
                    p += 2;
                    out->storage[w++] = e == 'n' ? '\n' : e;
                    continue;
                }
                out->storage[w++] = c;
                p++;
                continue;
            }
            if (c == ' ')
                break;
            if (c == '"') {
                quoted = true;
                p++;
                continue;
            }
            if (c == '\\' && p[1]) {
                out->storage[w++] = p[1];
                p += 2;
                continue;
            }
            out->storage[w++] = c;
            p++;
        }
        if (quoted)
            return CMDLINE_UNTERMINATED;
        out->storage[w++] = '\0';
    }
    return CMDLINE_OK;
}
