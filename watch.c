#include "watch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define WATCH_INITIAL_BUCKETS 1024

static hash_t wd_hash(const void *key)
{
    return (hash_t)*(const int *)key;
}

static int wd_cmp(const struct ht_node *n, const void *key)
{
    return container_of(n, watch_entry, node)->wd - *(const int *)key;
}

static watch_entry *watch_entry_find(watch_state *ws, int wd)
{
    struct ht_node *n = ht_find(&ws->watches, &wd);
    return n ? container_of(n, watch_entry, node) : NULL;
}

static void watch_entry_free(watch_entry *w)
{
    free(w->path);
    free(w);
}

/* On failure ws is still safe to pass to watch_destroy. */
int watch_init(watch_state *ws)
{
    ws->inotify_fd = -1;
    pthread_mutex_init(&ws->lock, NULL);
    return ht_init(&ws->watches, wd_hash, wd_cmp, WATCH_INITIAL_BUCKETS);
}

int watch_open(watch_state *ws)
{
    ws->inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ws->inotify_fd == -1) {
        perror("inotify_init1");
        return -1;
    }
    return 0;
}

void watch_destroy(watch_state *ws)
{
    watch_entry *w;
    struct hlist_node *tmp;
    uint32_t i;

    pthread_mutex_lock(&ws->lock);
    ht_for_each_entry_safe(w, tmp, i, &ws->watches, node) {
        inotify_rm_watch(ws->inotify_fd, w->wd);
        ht_remove(&ws->watches, &w->node);
        watch_entry_free(w);
    }
    ht_destroy(&ws->watches);
    pthread_mutex_unlock(&ws->lock);
    pthread_mutex_destroy(&ws->lock);
    if (ws->inotify_fd >= 0)
        close(ws->inotify_fd);
    ws->inotify_fd = -1;
}

/* True when old no longer names the directory that fresh does. */
static bool path_stale(const char *old, const char *fresh)
{
    struct stat o, f;
    if (stat(old, &o))
        return true;
    if (stat(fresh, &f))
        return false;
    return o.st_ino != f.st_ino || o.st_dev != f.st_dev;
}

int watch_add(watch_state *ws, const char *path)
{
    int wd = inotify_add_watch(ws->inotify_fd, path,
                               IN_CREATE | IN_DELETE | IN_OPEN | IN_CLOSE_WRITE |
                               IN_MOVED_FROM | IN_MOVED_TO |
                               IN_DELETE_SELF | IN_MOVE_SELF);
    if (wd == -1) {
        perror("inotify_add_watch");
        return -1;
    }

    watch_entry *w = malloc(sizeof(*w));
    if (!w)
        goto fail;
    w->wd = wd;
    w->path = strdup(path);
    if (!w->path) {
        free(w);
        goto fail;
    }

    /* The kernel returns the existing wd for a directory that is already watched. Usually the
     * entry then carries the same path (a second scan of the directory); if the old path no longer
     * leads to this inode, the directory was renamed while the events saying so were lost, and the
     * entry takes the new name. Two live paths for one inode (a bind mount) keep the old one.
     */
    pthread_mutex_lock(&ws->lock);
    watch_entry *old = watch_entry_find(ws, wd);
    if (!old) {
        if (ht_insert(&ws->watches, &w->node, &wd) == -1)
            watch_entry_free(w);
    } else {
        if (strcmp(old->path, path) && path_stale(old->path, path)) {
            char *tmp = old->path;
            old->path = w->path;
            w->path = tmp;
        }
        watch_entry_free(w);
    }
    pthread_mutex_unlock(&ws->lock);
    return 0;

fail:
    perror("watch_add");
    inotify_rm_watch(ws->inotify_fd, wd);
    return -1;
}

int watch_lookup(watch_state *ws, int wd, char *buf, size_t size)
{
    int rc = -1;

    pthread_mutex_lock(&ws->lock);
    watch_entry *w = watch_entry_find(ws, wd);
    if (w && (size_t)snprintf(buf, size, "%s", w->path) < size)
        rc = 0;
    pthread_mutex_unlock(&ws->lock);
    return rc;
}

void watch_prune(watch_state *ws)
{
    watch_entry *w;
    struct hlist_node *tmp;
    uint32_t i;
    struct stat st;

    pthread_mutex_lock(&ws->lock);
    ht_for_each_entry_safe(w, tmp, i, &ws->watches, node) {
        if (stat(w->path, &st)) {
            inotify_rm_watch(ws->inotify_fd, w->wd);   /* usually gone already: EINVAL, ignored */
            ht_remove(&ws->watches, &w->node);
            watch_entry_free(w);
        }
    }
    pthread_mutex_unlock(&ws->lock);
}

void watch_drop(watch_state *ws, int wd)
{
    pthread_mutex_lock(&ws->lock);
    watch_entry *w = watch_entry_find(ws, wd);
    if (w) {
        ht_remove(&ws->watches, &w->node);
        watch_entry_free(w);
    }
    pthread_mutex_unlock(&ws->lock);
}

void watch_remove_subtree(watch_state *ws, const char *path)
{
    size_t path_len = strlen(path);
    watch_entry *w;
    struct hlist_node *tmp;
    uint32_t i;

    pthread_mutex_lock(&ws->lock);
    ht_for_each_entry_safe(w, tmp, i, &ws->watches, node) {
        if (!strncmp(w->path, path, path_len) &&
            (w->path[path_len] == '\0' || w->path[path_len] == '/')) {
            inotify_rm_watch(ws->inotify_fd, w->wd);
            ht_remove(&ws->watches, &w->node);
            watch_entry_free(w);
        }
    }
    pthread_mutex_unlock(&ws->lock);
}
