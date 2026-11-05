/* Path normalisation. Rules:
 *  - "/" is the root; relative paths are resolved against `cwd` (absolute).
 *  - Repeated and trailing slashes are ignored.
 *  - "." is dropped; ".." removes the previous component; ".." at the root
 *    stays at the root.
 *  - Components must be 1..63 chars from [A-Za-z0-9._-] and not "." or "..".
 */
#include "fs/vfs.h"
#include "lib.h"

bool vfs_valid_name(const char *name, size_t len)
{
    if (len == 0 || len > VFS_NAME_MAX)
        return false;
    if ((len == 1 && name[0] == '.') || (len == 2 && name[0] == '.' && name[1] == '.'))
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

static int process(const char *p, char *out, size_t cap, size_t *len)
{
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t n = (size_t)(p - start);
        if (n == 1 && start[0] == '.')
            continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (*len > 0 && out[*len - 1] != '/')
                (*len)--;
            if (*len > 0)
                (*len)--; /* remove the separating slash */
            continue;
        }
        if (n > VFS_NAME_MAX)
            return VFS_ENAMETOOLONG;
        if (!vfs_valid_name(start, n))
            return VFS_EINVAL;
        if (*len + 1 + n > VFS_PATH_MAX || *len + 1 + n + 1 > cap)
            return VFS_ENAMETOOLONG;
        out[(*len)++] = '/';
        memcpy(out + *len, start, n);
        *len += n;
    }
    return VFS_OK;
}

int vfs_normalize(const char *cwd, const char *path, char *out, size_t cap)
{
    if (!path || !out || cap < 2)
        return VFS_EINVAL;
    if (!path[0])
        return VFS_EINVAL;
    if (strlen(path) > 1024)
        return VFS_ENAMETOOLONG;
    size_t len = 0;
    int r;
    if (path[0] != '/') {
        if (!cwd || cwd[0] != '/')
            return VFS_EINVAL;
        r = process(cwd, out, cap, &len);
        if (r)
            return r;
    }
    r = process(path, out, cap, &len);
    if (r)
        return r;
    if (len == 0)
        out[len++] = '/';
    out[len] = '\0';
    return VFS_OK;
}

int vfs_split(const char *norm, char *parent, size_t pcap, char *name, size_t ncap)
{
    const char *slash = strrchr(norm, '/');
    if (!slash || norm[0] != '/' || norm[1] == '\0')
        return VFS_EINVAL;
    size_t plen = (size_t)(slash - norm);
    if (plen == 0)
        plen = 1; /* parent is root */
    if (plen + 1 > pcap || strlen(slash + 1) + 1 > ncap)
        return VFS_ENAMETOOLONG;
    memcpy(parent, norm, plen);
    parent[plen] = '\0';
    str_copy(name, slash + 1, ncap);
    return VFS_OK;
}
