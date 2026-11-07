/* hello: smallest Aurora program. */
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    printf("Hello from user space! pid=%d argc=%d", getpid(), argc);
    for (int i = 1; i < argc; i++)
        printf(" [%s]", argv[i]);
    printf("\n");
    return 0;
}
