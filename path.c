#define _GNU_SOURCE   /* statx */
#include "path.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

char *path_strdup_normalized(const char *path)
{
    char *copy = strdup(path);
    if (!copy)
        return NULL;

    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/')
        copy[--len] = '\0';

    return copy;
}

int path_join(char *buf, size_t size, const char *dir, const char *name)
{
    size_t dlen = strlen(dir);
    const char *sep = dlen && dir[dlen - 1] == '/' ? "" : "/";
    int n = snprintf(buf, size, "%s%s%s", dir, sep, name);
    return n < 0 || (size_t)n >= size ? -1 : n;
}

bool path_xdg_path(const char *var, const char *fallback, char *dir, size_t size)
{
    const char *base = getenv(var);
    const char *home = getenv("HOME");
    int n = base && base[0] ? snprintf(dir, size, "%s/desktop-search", base)
          : home && home[0] ? snprintf(dir, size, "%s/%s/desktop-search", home, fallback)
                            : -1;
    return n >= 0 && (size_t)n < size;
}

bool path_xdg_dir(const char *var, const char *fallback, char *dir, size_t size)
{
    if (!path_xdg_path(var, fallback, dir, size))
        return false;
    /* mkdir -p for the two levels that may be missing; an existing directory is fine. */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%.*s", (int)(strrchr(dir, '/') - dir), dir);
    mkdir(parent, 0700);
    return !mkdir(dir, 0700) || errno == EEXIST;
}

bool path_identity(const char *path, uint64_t *dev, uint64_t *ino, int64_t *btime)
{
    struct statx stx;
    if (statx(AT_FDCWD, path, 0, STATX_INO | STATX_BTIME, &stx))
        return false;
    *dev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
    *ino = stx.stx_ino;
    *btime = stx.stx_mask & STATX_BTIME ? (int64_t)stx.stx_btime.tv_sec * 1000000000 + stx.stx_btime.tv_nsec : 0;
    return true;
}
