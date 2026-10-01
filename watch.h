#ifndef _WATCH_H
#define _WATCH_H

#include <pthread.h>

#include "htable.h"

typedef struct watch_entry {
    int wd;
    char *path;              /* heap-allocated, exact length */
    struct ht_node node;
} watch_entry;

/* inotify descriptors are allocated cyclically by the kernel and never reused until they wrap, so
 * they are not usable as a dense array index; the table keeps memory proportional to live watches.
 */
typedef struct watch_state {
    int inotify_fd;
    struct htable watches;   /* wd → watch_entry */
    pthread_mutex_t lock;
} watch_state;

/* The table only; there is no inotify fd until watch_open (a one-off --query walk needs none). */
int watch_init(watch_state *ws);

/* Creates the inotify fd (non-blocking). watch_destroy closes it. */
int watch_open(watch_state *ws);
void watch_destroy(watch_state *ws);

/* Watches path. A directory is identified by its inode, so a directory that is already watched
 * under another name (renamed while the events that would have said so were lost to a queue
 * overflow) comes back with the same descriptor; the entry then takes the name that currently
 * reaches the inode.
 */
int watch_add(watch_state *ws, const char *path);

/* Copies the watched path of wd into buf. Returns -1 if wd is unknown or the path does not fit.
 * The copy is the caller's: the table may rename or drop the entry the moment this returns.
 */
int watch_lookup(watch_state *ws, int wd, char *buf, size_t size);

/* Drops the entries whose path no longer exists: after a queue overflow the IN_IGNORED of a
 * removed directory may never have arrived. A directory deleted and recreated under the same name
 * keeps its dead entry (the path exists), which costs memory and nothing else, as its descriptor
 * never fires again.
 */
void watch_prune(watch_state *ws);

/* Forget a watch the kernel has already removed (IN_IGNORED). */
void watch_drop(watch_state *ws, int wd);

void watch_remove_subtree(watch_state *ws, const char *path);

#endif
