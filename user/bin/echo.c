/* echo [-n] [args...] */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    int i = 1, nl = 1;
    if (i < argc && !strcmp(argv[i], "-n")) {
        nl = 0;
        i++;
    }
    for (int first = i; i < argc; i++)
        printf("%s%s", i > first ? " " : "", argv[i]);
    if (nl)
        putchar('\n');
    return 0;
}
