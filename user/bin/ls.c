/* ls [dir]: lists a directory through the readdir system call. */
#include "aurora.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/home";
    struct aurora_dirent d;
    uint32_t i = 0;
    int r;
    while ((r = readdir(path, i, &d)) == 0) {
        printf("%-30s %s %6u%s\n", d.name, d.type == 2 ? "dir " : "file", d.size, d.readonly ? "  ro" : "");
        i++;
    }
    if (r != -1) {
        printf("ls: %s: %s\n", path, strerror(r));
        return 1;
    }
    if (i == 0)
        printf("(empty)\n");
    return 0;
}
