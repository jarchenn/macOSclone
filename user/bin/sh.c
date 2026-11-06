/* sh: Aurora's POSIX-style shell (0.6), original code.
 *
 * Supports: pipelines (a | b | c), redirection (< > >> 2> 2>> 2>&1 >&2),
 * lists (; && || and & for background jobs), single and double quotes,
 * backslash escapes, $VAR ${VAR} $? $$ $!, filename globbing (* ? [...]),
 * comments, and the builtins cd, pwd, exit, export, unset, set, wait, jobs,
 * true/false, umask, exec and `.`.
 * Usage: sh [-c command | script [args...]]
 *
 * Commands run with fork + execvp. An interactive shell catches SIGINT
 * (Ctrl-C kills the foreground program, not the shell); children start with
 * default dispositions because exec resets caught signals. Job control
 * (stopping, fg/bg) is not implemented, as the 0.6 roadmap defers it. */
#define _GNU_SOURCE /* pipe2 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAX_WORDS 128
#define MAX_CMDS 16
#define MAX_REDIRS 8

extern char **environ;

static int last_status;
static pid_t last_bg;
static bool interactive;
static volatile sig_atomic_t got_sigint;

/* ---- tokens --------------------------------------------------------------- */

enum tok { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG, T_LT, T_GT, T_GTGT, T_ERR_GT, T_ERR_GTGT, T_ERR_TO_OUT,
           T_OUT_TO_ERR, T_END };

struct token {
    enum tok kind;
    char *text;  /* expanded word */
    bool glob;   /* contains unquoted glob characters */
};

static struct token toks[512];
static int ntok;

static char *xstrdup(const char *s)
{
    char *d = strdup(s);
    if (!d) {
        fprintf(stderr, "sh: out of memory\n");
        exit(2);
    }
    return d;
}

struct buf {
    char *p;
    size_t n, cap;
};

static void bput(struct buf *b, char c)
{
    if (b->n + 2 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = realloc(b->p, b->cap);
        if (!b->p)
            exit(2);
    }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
}

static void bputs(struct buf *b, const char *s)
{
    while (*s)
        bput(b, *s++);
}

static const char *var(const char *name)
{
    static char num[32];
    if (!strcmp(name, "?")) {
        snprintf(num, sizeof num, "%d", last_status);
        return num;
    }
    if (!strcmp(name, "$")) {
        snprintf(num, sizeof num, "%d", (int)getpid());
        return num;
    }
    if (!strcmp(name, "!")) {
        snprintf(num, sizeof num, "%d", (int)last_bg);
        return num;
    }
    const char *v = getenv(name);
    return v ? v : "";
}

/* Reads a $-expansion at *pp (after the '$') into b. */
static void expand_dollar(const char **pp, struct buf *b)
{
    const char *p = *pp;
    char name[64];
    size_t n = 0;
    if (*p == '{') {
        p++;
        while (*p && *p != '}' && n + 1 < sizeof name)
            name[n++] = *p++;
        if (*p == '}')
            p++;
    } else if (*p == '?' || *p == '$' || *p == '!') {
        name[n++] = *p++;
    } else {
        while ((isalnum((unsigned char)*p) || *p == '_') && n + 1 < sizeof name)
            name[n++] = *p++;
    }
    name[n] = 0;
    if (n == 0)
        bput(b, '$');
    else
        bputs(b, var(name));
    *pp = p;
}

static bool add_tok(enum tok k, char *text, bool g)
{
    if (ntok >= (int)(sizeof toks / sizeof toks[0]) - 1) {
        fprintf(stderr, "sh: line too long\n");
        return false;
    }
    toks[ntok].kind = k;
    toks[ntok].text = text;
    toks[ntok].glob = g;
    ntok++;
    return true;
}

static bool tokenize(const char *p)
{
    ntok = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (!*p || *p == '#')
            break;
        if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') {
            add_tok(T_ERR_TO_OUT, NULL, false);
            p += 4;
            continue;
        }
        if (p[0] == '>' && p[1] == '&' && p[2] == '2') {
            add_tok(T_OUT_TO_ERR, NULL, false);
            p += 3;
            continue;
        }
        if (p[0] == '2' && p[1] == '>') {
            bool app = p[2] == '>';
            add_tok(app ? T_ERR_GTGT : T_ERR_GT, NULL, false);
            p += app ? 3 : 2;
            continue;
        }
        switch (*p) {
        case '|':
            if (p[1] == '|') { add_tok(T_OR, NULL, false); p += 2; } else { add_tok(T_PIPE, NULL, false); p++; }
            continue;
        case '&':
            if (p[1] == '&') { add_tok(T_AND, NULL, false); p += 2; } else { add_tok(T_BG, NULL, false); p++; }
            continue;
        case ';': add_tok(T_SEMI, NULL, false); p++; continue;
        case '<': add_tok(T_LT, NULL, false); p++; continue;
        case '>':
            if (p[1] == '>') { add_tok(T_GTGT, NULL, false); p += 2; } else { add_tok(T_GT, NULL, false); p++; }
            continue;
        default: break;
        }
        struct buf b = { 0 };
        bool g = false, any = false;
        while (*p && !strchr(" \t\r\n|&;<>", *p)) {
            any = true;
            if (*p == '\\' && p[1]) {
                bput(&b, p[1]);
                p += 2;
            } else if (*p == '\'') {
                p++;
                while (*p && *p != '\'')
                    bput(&b, *p++);
                if (*p != '\'') {
                    fprintf(stderr, "sh: unterminated quote\n");
                    free(b.p);
                    return false;
                }
                p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
                        bput(&b, p[1]);
                        p += 2;
                    } else if (*p == '$') {
                        p++;
                        expand_dollar(&p, &b);
                    } else {
                        bput(&b, *p++);
                    }
                }
                if (*p != '"') {
                    fprintf(stderr, "sh: unterminated quote\n");
                    free(b.p);
                    return false;
                }
                p++;
            } else if (*p == '$') {
                p++;
                expand_dollar(&p, &b);
            } else {
                if (*p == '*' || *p == '?' || *p == '[')
                    g = true;
                bput(&b, *p++);
            }
        }
        if (any && !add_tok(T_WORD, b.p ? b.p : xstrdup(""), g))
            return false;
    }
    toks[ntok].kind = T_END;
    return true;
}

static void free_tokens(void)
{
    for (int i = 0; i < ntok; i++)
        free(toks[i].text);
    ntok = 0;
}

/* ---- commands ---------------------------------------------------------------- */

struct redir {
    enum tok kind;
    const char *path;
};

struct command {
    char *argv[MAX_WORDS + 1];
    int argc;
    struct redir r[MAX_REDIRS];
    int nr;
    glob_t globs[8];
    int nglob;
};

static void free_command(struct command *c)
{
    for (int i = 0; i < c->nglob; i++)
        globfree(&c->globs[i]);
    c->nglob = 0;
}

/* Parses a pipeline element from toks[*i]; stops at | ; & && || or end. */
static bool parse_command(int *i, struct command *c)
{
    memset(c, 0, sizeof *c);
    while (toks[*i].kind != T_END && toks[*i].kind != T_PIPE && toks[*i].kind != T_SEMI &&
           toks[*i].kind != T_BG && toks[*i].kind != T_AND && toks[*i].kind != T_OR) {
        struct token *t = &toks[*i];
        if (t->kind == T_WORD) {
            if (t->glob && c->nglob < 8) {
                glob_t *g = &c->globs[c->nglob];
                if (glob(t->text, 0, NULL, g) == 0) {
                    c->nglob++;
                    for (size_t k = 0; k < (size_t)g->gl_pathc && c->argc < MAX_WORDS; k++)
                        c->argv[c->argc++] = g->gl_pathv[k];
                    (*i)++;
                    continue;
                }
                globfree(g);
            }
            if (c->argc < MAX_WORDS)
                c->argv[c->argc++] = t->text;
            (*i)++;
            continue;
        }
        if (t->kind == T_ERR_TO_OUT || t->kind == T_OUT_TO_ERR) {
            if (c->nr < MAX_REDIRS)
                c->r[c->nr++] = (struct redir){ t->kind, NULL };
            (*i)++;
            continue;
        }
        /* < > >> 2> 2>> need a file name */
        if (toks[*i + 1].kind != T_WORD) {
            fprintf(stderr, "sh: syntax error: redirection without a file name\n");
            return false;
        }
        if (c->nr < MAX_REDIRS)
            c->r[c->nr++] = (struct redir){ t->kind, toks[*i + 1].text };
        *i += 2;
    }
    c->argv[c->argc] = NULL;
    return true;
}

static int apply_redirs(const struct command *c)
{
    for (int k = 0; k < c->nr; k++) {
        const struct redir *r = &c->r[k];
        int fd = -1, target;
        switch (r->kind) {
        case T_LT: fd = open(r->path, O_RDONLY); target = 0; break;
        case T_GT: fd = open(r->path, O_WRONLY | O_CREAT | O_TRUNC, 0644); target = 1; break;
        case T_GTGT: fd = open(r->path, O_WRONLY | O_CREAT | O_APPEND, 0644); target = 1; break;
        case T_ERR_GT: fd = open(r->path, O_WRONLY | O_CREAT | O_TRUNC, 0644); target = 2; break;
        case T_ERR_GTGT: fd = open(r->path, O_WRONLY | O_CREAT | O_APPEND, 0644); target = 2; break;
        case T_ERR_TO_OUT: dup2(1, 2); continue;
        case T_OUT_TO_ERR: dup2(2, 1); continue;
        default: continue;
        }
        if (fd < 0) {
            fprintf(stderr, "sh: %s: %s\n", r->path, strerror(errno));
            return -1;
        }
        if (fd != target) {
            dup2(fd, target);
            close(fd);
        }
    }
    return 0;
}

/* ---- builtins ---------------------------------------------------------------- */

static void reap_background(bool report)
{
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0)
        if (report && interactive)
            fprintf(stderr, "[%d] done (status %d)\n", (int)p, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st));
}

static int run_script(FILE *f);

static bool is_builtin(const char *n)
{
    static const char *const names[] = { "cd", "pwd", "exit", "export", "unset", "set", "wait", "jobs",
                                         "true", "false", ":", "umask", "exec", ".", NULL };
    for (int i = 0; names[i]; i++)
        if (!strcmp(n, names[i]))
            return true;
    return false;
}

static int builtin(struct command *c)
{
    char **a = c->argv;
    if (!strcmp(a[0], "cd")) {
        const char *d = c->argc > 1 ? a[1] : getenv("HOME");
        if (!d)
            d = "/";
        if (chdir(d) < 0) {
            fprintf(stderr, "cd: %s: %s\n", d, strerror(errno));
            return 1;
        }
        char cwd[256];
        if (getcwd(cwd, sizeof cwd))
            setenv("PWD", cwd, 1);
        return 0;
    }
    if (!strcmp(a[0], "pwd")) {
        char cwd[256];
        if (!getcwd(cwd, sizeof cwd)) {
            perror("pwd");
            return 1;
        }
        printf("%s\n", cwd);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(a[0], "exit")) {
        fflush(stdout);
        exit(c->argc > 1 ? atoi(a[1]) : last_status);
    }
    if (!strcmp(a[0], "export") || !strcmp(a[0], "set")) {
        if (c->argc == 1) {
            for (char **e = environ; *e; e++)
                printf("%s%s\n", a[0][0] == 'e' ? "export " : "", *e);
            fflush(stdout);
            return 0;
        }
        for (int i = 1; i < c->argc; i++) {
            char *eq = strchr(a[i], '=');
            if (!eq)
                continue;
            *eq = 0;
            setenv(a[i], eq + 1, 1);
            *eq = '=';
        }
        return 0;
    }
    if (!strcmp(a[0], "unset")) {
        for (int i = 1; i < c->argc; i++)
            unsetenv(a[i]);
        return 0;
    }
    if (!strcmp(a[0], "wait")) {
        int st = 0;
        if (c->argc > 1) {
            pid_t p = (pid_t)atoi(a[1]);
            while (waitpid(p, &st, 0) < 0 && errno == EINTR)
                ;
        } else {
            while (wait(&st) > 0 || errno == EINTR)
                ;
        }
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    if (!strcmp(a[0], "jobs")) {
        reap_background(true);
        return 0;
    }
    if (!strcmp(a[0], "true") || !strcmp(a[0], ":"))
        return 0;
    if (!strcmp(a[0], "false"))
        return 1;
    if (!strcmp(a[0], "umask")) {
        if (c->argc > 1)
            umask((mode_t)strtol(a[1], NULL, 8));
        else {
            mode_t m = umask(0);
            umask(m);
            printf("%04o\n", (unsigned)m);
            fflush(stdout);
        }
        return 0;
    }
    if (!strcmp(a[0], "exec")) {
        if (c->argc < 2)
            return apply_redirs(c) < 0;
        execvp(a[1], a + 1);
        fprintf(stderr, "sh: exec %s: %s\n", a[1], strerror(errno));
        return 127;
    }
    if (!strcmp(a[0], ".")) {
        if (c->argc < 2)
            return 0;
        FILE *f = fopen(a[1], "r");
        if (!f) {
            fprintf(stderr, "sh: %s: %s\n", a[1], strerror(errno));
            return 1;
        }
        int r = run_script(f);
        fclose(f);
        return r;
    }
    return 127;
}

/* ---- execution ---------------------------------------------------------------- */

static int status_of(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    return 128 + WTERMSIG(st);
}

static void report_signal(int st)
{
    if (!WIFSIGNALED(st))
        return;
    int s = WTERMSIG(st);
    if (s == SIGINT)
        fputc('\n', stderr);
    else if (s != SIGPIPE)
        fprintf(stderr, "%s\n", strsignal(s));
}

static int run_pipeline(struct command *cmds, int n, bool background)
{
    /* A lone builtin runs in the shell itself (cd, export, exit...). */
    if (n == 1 && cmds[0].argc > 0 && is_builtin(cmds[0].argv[0]) && !background) {
        int save[3] = { dup(0), dup(1), dup(2) };
        int r = apply_redirs(&cmds[0]) < 0 ? 1 : builtin(&cmds[0]);
        fflush(stdout);
        fflush(stderr);
        for (int k = 0; k < 3; k++) {
            dup2(save[k], k);
            close(save[k]);
        }
        return r;
    }
    pid_t pids[MAX_CMDS];
    int prev = -1;
    for (int i = 0; i < n; i++) {
        int pfd[2] = { -1, -1 };
        if (i + 1 < n && pipe2(pfd, O_CLOEXEC) < 0) {
            fprintf(stderr, "sh: pipe: %s\n", strerror(errno));
            return 1;
        }
        fflush(stdout);
        pid_t p = fork();
        if (p < 0) {
            fprintf(stderr, "sh: fork: %s\n", strerror(errno));
            return 1;
        }
        if (p == 0) {
            signal(SIGINT, SIG_DFL);
            signal(SIGQUIT, SIG_DFL);
            if (background && !interactive)
                signal(SIGINT, SIG_IGN);
            if (prev >= 0) {
                dup2(prev, 0);
                close(prev);
            }
            if (pfd[1] >= 0) {
                dup2(pfd[1], 1);
                close(pfd[1]);
                close(pfd[0]);
            }
            if (apply_redirs(&cmds[i]) < 0)
                _exit(1);
            if (cmds[i].argc == 0)
                _exit(0);
            if (is_builtin(cmds[i].argv[0])) {
                int r = builtin(&cmds[i]);
                fflush(stdout);
                _exit(r);
            }
            execvp(cmds[i].argv[0], cmds[i].argv);
            fprintf(stderr, "sh: %s: %s\n", cmds[i].argv[0], errno == ENOENT ? "command not found" : strerror(errno));
            _exit(errno == ENOENT ? 127 : 126);
        }
        pids[i] = p;
        if (prev >= 0)
            close(prev);
        if (pfd[1] >= 0)
            close(pfd[1]);
        prev = pfd[0];
    }
    if (background) {
        last_bg = pids[n - 1];
        if (interactive)
            fprintf(stderr, "[%d]\n", (int)last_bg);
        return 0;
    }
    int status = 0;
    for (int i = 0; i < n; i++) {
        int st = 0;
        while (waitpid(pids[i], &st, 0) < 0) {
            if (errno != EINTR) {
                st = 0;
                break;
            }
        }
        if (i == n - 1) {
            status = status_of(st);
            report_signal(st);
        }
    }
    return status;
}

/* Runs one parsed line: pipelines separated by ; & && ||. */
static int run_line(const char *line)
{
    if (!tokenize(line))
        return 2;
    int i = 0;
    int status = last_status;
    enum tok pending = T_SEMI; /* how the next pipeline is joined to the previous */
    while (toks[i].kind != T_END) {
        struct command cmds[MAX_CMDS];
        int n = 0;
        bool ok = true;
        for (;;) {
            if (n >= MAX_CMDS) {
                fprintf(stderr, "sh: pipeline too long\n");
                ok = false;
                break;
            }
            if (!parse_command(&i, &cmds[n])) {
                ok = false;
                break;
            }
            n++;
            if (toks[i].kind != T_PIPE)
                break;
            i++;
        }
        enum tok sep = toks[i].kind;
        if (sep != T_END)
            i++;
        bool run = ok && (pending == T_SEMI || pending == T_BG || (pending == T_AND && status == 0) ||
                          (pending == T_OR && status != 0));
        if (run && !(n == 1 && cmds[0].argc == 0 && cmds[0].nr == 0)) {
            status = run_pipeline(cmds, n, sep == T_BG);
            last_status = status;
        }
        for (int k = 0; k < n; k++)
            free_command(&cmds[k]);
        if (!ok) {
            status = last_status = 2;
            break;
        }
        pending = sep;
    }
    free_tokens();
    return status;
}

static int run_script(FILE *f)
{
    char line[1024];
    int st = 0;
    while (fgets(line, sizeof line, f))
        st = run_line(line);
    return st;
}

static void on_sigint(int s)
{
    (void)s;
    got_sigint = 1;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "-c"))
        return run_line(argv[2]);
    if (argc > 1) {
        FILE *f = fopen(argv[1], "r");
        if (!f) {
            fprintf(stderr, "sh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
        int r = run_script(f);
        fclose(f);
        return r;
    }
    interactive = isatty(0);
    if (interactive) {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_sigint;
        sigaction(SIGINT, &sa, NULL); /* no SA_RESTART: reads see EINTR */
        signal(SIGQUIT, SIG_IGN);
        printf("Aurora sh (pid %d). Pipelines, redirection, $VARS; 'exit' to leave.\n", (int)getpid());
    }
    char line[1024];
    for (;;) {
        reap_background(true);
        if (interactive) {
            char cwd[256];
            if (!getcwd(cwd, sizeof cwd))
                strcpy(cwd, "?");
            printf("%s %s ", cwd, getuid() == 0 ? "#" : "$");
            fflush(stdout);
        }
        got_sigint = 0;
        if (!fgets(line, sizeof line, stdin)) {
            if (ferror(stdin) && errno == EINTR) {
                clearerr(stdin);
                continue; /* Ctrl-C at the prompt: new prompt */
            }
            break;
        }
        run_line(line);
    }
    if (interactive)
        printf("exit\n");
    return last_status;
}
