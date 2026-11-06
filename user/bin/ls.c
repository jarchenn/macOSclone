/* ls [-la] [path...]: lists directories (opendir/readdir) or files. */
#include <dirent.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static int opt_l, opt_a;

static void mode_string(mode_t m, char *s)
{
    s[0] = S_ISDIR(m) ? 'd' : S_ISCHR(m) ? 'c' : S_ISFIFO(m) ? 'p' : '-';
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        s[1 + i] = (m & (0400 >> i)) ? rwx[i] : '-';
    if (m & 01000)
        s[9] = (m & 1) ? 't' : 'T';
    s[10] = 0;
}

static void show(const char *dir, const char *name)
{
    if (!opt_l) {
        printf("%s\n", name);
        return;
    }
    char path[512];
    if (dir)
        snprintf(path, sizeof path, "%s/%s", dir, name);
    else
        snprintf(path, sizeof path, "%s", name);
    struct stat st;
    if (stat(path, &st) < 0) {
        printf("?????????? %s\n", name);
        return;
    }
    char m[12], when[32];
    mode_string(st.st_mode, m);
    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    time_t t = st.st_mtim.tv_sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    printf("%s %-5s %-5s %8ld %s %s%s\n", m, pw ? pw->pw_name : "?", gr ? gr->gr_name : "?", (long)st.st_size,
           when, name, S_ISDIR(st.st_mode) ? "/" : "");
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int list(const char *path, int header)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        show(NULL, path);
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && !opt_a)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp);
    if (header)
        printf("%s:\n", path);
    for (size_t i = 0; i < n; i++) {
        show(path, names[i]);
        free(names[i]);
    }
    free(names);
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        for (const char *o = argv[first] + 1; *o; o++) {
            if (*o == 'l')
                opt_l = 1;
            else if (*o == 'a')
                opt_a = 1;
            else {
                fprintf(stderr, "ls: unknown option -%c\n", *o);
                return 2;
            }
        }
        first++;
    }
    if (first >= argc)
        return list(".", 0);
    int status = 0;
    for (int i = first; i < argc; i++)
        status |= list(argv[i], argc - first > 1);
    return status;
}
