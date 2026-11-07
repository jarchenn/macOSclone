/* upper: echoes input lines in upper case until end-of-input (Ctrl+D). */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char line[256];
    long lines = 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("upper: type lines, Ctrl+D to finish\n");
    while (fgets(line, sizeof line, stdin)) {
        for (char *p = line; *p; p++)
            *p = (char)toupper((unsigned char)*p);
        fputs(line, stdout);
        if (!strchr(line, '\n'))
            putchar('\n');
        lines++;
    }
    printf("upper: %ld lines\n", lines);
    return 0;
}
