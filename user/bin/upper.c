/* upper: echoes console input in upper case until end-of-input (Ctrl+D). */
#include "aurora.h"

int main(void)
{
    char line[256];
    long n, lines = 0;
    printf("upper: type lines, Ctrl+D to finish\n");
    while ((n = readline(line, sizeof line)) >= 0) {
        for (long i = 0; i < n; i++)
            if (line[i] >= 'a' && line[i] <= 'z')
                line[i] = (char)(line[i] - 32);
        printf("%s\n", line);
        lines++;
    }
    printf("upper: %ld lines\n", lines);
    return 0;
}
