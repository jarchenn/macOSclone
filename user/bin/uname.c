/* uname [-a|-s|-r|-v|-m|-n] */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv)
{
    struct utsname u;
    if (uname(&u) < 0) {
        perror("uname");
        return 1;
    }
    const char *o = argc > 1 ? argv[1] : "-s";
    if (!strcmp(o, "-a"))
        printf("%s %s %s %s %s\n", u.sysname, u.nodename, u.release, u.version, u.machine);
    else if (!strcmp(o, "-r"))
        printf("%s\n", u.release);
    else if (!strcmp(o, "-v"))
        printf("%s\n", u.version);
    else if (!strcmp(o, "-m"))
        printf("%s\n", u.machine);
    else if (!strcmp(o, "-n"))
        printf("%s\n", u.nodename);
    else
        printf("%s\n", u.sysname);
    return 0;
}
