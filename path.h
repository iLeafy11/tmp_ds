#ifndef _PATH_H
#define _PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Path helpers: nothing here knows about the index. */

/* Copy of path with trailing slashes removed ("/" stays "/"). NULL on allocation failure. */
char *path_strdup_normalized(const char *path);

/* Writes "<dir>/<name>" into buf; a dir that already ends in '/' (the root "/") gets no second
 * slash, so "/" + "alpha" is "/alpha", never "//alpha". An empty name yields the "<dir>/" prefix
 * that selects everything below dir.
 *
 * Returns the length, or -1 if it does not fit.
 */
int path_join(char *buf, size_t size, const char *dir, const char *name);

/* The identity of a directory on disk, which a rename keeps and a deletion plus recreation does
 * not: device, inode and birth time in nanoseconds (0 when the filesystem reports none, and then
 * the identity is only as good as an inode number, which is reused). Returns false if path cannot
 * be stat'ed.
 */
bool path_identity(const char *path, uint64_t *dev, uint64_t *ino, int64_t *btime);

/* "$VAR/desktop-search", or "~/<fallback>/desktop-search" when the variable is not set: an XDG base
 * directory ($XDG_STATE_HOME holds the logs and the snapshots). Returns false when neither the
 * variable nor HOME is set or the path does not fit. path_xdg_path only computes the path;
 * path_xdg_dir also creates the directory.
 */
bool path_xdg_path(const char *var, const char *fallback, char *dir, size_t size);
bool path_xdg_dir(const char *var, const char *fallback, char *dir, size_t size);

#endif /* _PATH_H */
