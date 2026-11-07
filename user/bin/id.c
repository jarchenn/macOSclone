/* id: prints the user and group of this process. */
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    uid_t u = getuid();
    gid_t g = getgid();
    struct passwd *pw = getpwuid(u);
    struct group *gr = getgrgid(g);
    printf("uid=%u(%s) gid=%u(%s)\n", (unsigned)u, pw ? pw->pw_name : "?", (unsigned)g, gr ? gr->gr_name : "?");
    return 0;
}
