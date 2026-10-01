#include "scan.h"
#include "watch.h"
#include "threadpool.h"
#include "path.h"
#include "utf8.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>

/* Work handed to the pool: a directory to scan. */
typedef struct path_task {
    search_ctx *ctx;
    char *path;
} path_task;

static void path_task_free(path_task *task)
{
    free(task->path);
    free(task);
}

/* Directory scan
 *
 * Names are filtered the same way here and in handle_event: hidden names and names that are not
 * well-formed UTF-8 are skipped (a directory with such a name takes its subtree with it). Linux
 * allows any bytes in a name, but every path the index holds then survives the JSON reply
 * unchanged, so a client can hand it back. The two filters must agree or a file created while
 * running would be found now and swept at the next start.
 */

static void scan_task(void *arg);

/* A directory the scan cannot get to is not a deleted one: whatever the index holds for it stays as
 * it was, so that the sweep after a diff scan leaves it alone.
 */
int scan_submit(search_ctx *ctx, const char *path)
{
    path_task *task = malloc(sizeof(*task));
    char *copy = path_strdup_normalized(path);
    if (task && copy) {
        task->ctx = ctx;
        task->path = copy;
        if (!tp_submit(ctx->tp, scan_task, task))
            return 0;
    }
    free(copy);
    free(task);
    ctx_mark_subtree_seen(ctx, path);
    return -1;
}

static unsigned char scan_entry_type(DIR *dir, const struct dirent *entry)
{
    if (entry->d_type != DT_UNKNOWN)
        return entry->d_type;

    struct stat st;
    if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) == -1)
        return DT_UNKNOWN;
    if (S_ISDIR(st.st_mode))
        return DT_DIR;
    if (S_ISREG(st.st_mode))
        return DT_REG;
    return DT_UNKNOWN;
}

static void scan_task(void *arg)
{
    path_task *task = arg;
    search_ctx *ctx = task->ctx;
    DIR *dir = NULL;

    if (ctx->shutdown)
        goto out;
    if (!(dir = opendir(task->path))) {
        ctx_mark_subtree_seen(ctx, task->path);
        goto out;
    }

    /* No inotify at all (--query walks once and leaves) means nothing to watch with. */
    if (ctx->ws.inotify_fd >= 0 && watch_add(&ctx->ws, task->path))
        atomic_fetch_add(&ctx->unwatched, 1);
    ctx_insert(ctx, task->path, true);

    /* readdir returns NULL both at the end and on error; only errno tells them apart, so it is
     * cleared right before each call (the body's own calls may leave it set).
     */
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry)
            break;
        if (entry->d_name[0] == '.' || !utf8_valid(entry->d_name))
            continue;
        unsigned char type = scan_entry_type(dir, entry);
        if (type != DT_DIR && type != DT_REG)
            continue;

        char full_path[PATH_MAX];
        if (path_join(full_path, sizeof(full_path), task->path, entry->d_name) < 0)
            continue;
        if (type == DT_DIR)
            scan_submit(ctx, full_path);
        else
            ctx_insert(ctx, full_path, false);
    }
    if (errno)
        ctx_mark_subtree_seen(ctx, task->path);

    /* The directory may have been removed or moved away while it was being listed; its delete event
     * was then applied before some of the inserts above. A removed directory has no links left, a
     * moved one is no longer what the path names. Either way what was just inserted is withdrawn; a
     * removal after this check is handled by its own event, which now follows the inserts.
     */
    struct stat open_st, path_st;
    if (!fstat(dirfd(dir), &open_st) &&
        (open_st.st_nlink == 0 || stat(task->path, &path_st) ||
         path_st.st_ino != open_st.st_ino || path_st.st_dev != open_st.st_dev))
        ctx_delete_subtree(ctx, task->path);
    closedir(dir);

out:
    path_task_free(task);
}

/* inotify event handling */

static void handle_event(search_ctx *ctx, const struct inotify_event *event,
                         const char *dir)
{
    if (!event->len || event->name[0] == '.' || !utf8_valid(event->name))
        return;

    char full_path[PATH_MAX];
    if (path_join(full_path, sizeof(full_path), dir, event->name) < 0)
        return;

    /* A new directory may already contain entries written before our watch is in place, so it is
     * scanned rather than merely inserted; scan_task adds the watch and indexes the directory
     * itself. Anything else is indexed only if it is a regular file, as the scan keeps just DT_DIR
     * and DT_REG. The lstat never follows a link, so a symlink to a directory is neither entered
     * (loops, escaping the root) nor listed.
     */
    if (event->mask & (IN_CREATE | IN_MOVED_TO)) {
        struct stat st;
        if (event->mask & IN_ISDIR)
            scan_submit(ctx, full_path);
        else if (!fstatat(AT_FDCWD, full_path, &st, AT_SYMLINK_NOFOLLOW) && S_ISREG(st.st_mode))
            ctx_insert(ctx, full_path, false);
    }

    /* Deletes are applied here, in event order, never handed to the pool: a subtree delete that ran
     * on a worker could complete after the scan of a directory recreated under the same name and
     * take the new contents with it. Marking a subtree dead is O(subtree) with no token index work,
     * so it is cheap enough for the event loop.
     */
    if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
        if (event->mask & IN_ISDIR) {
            watch_remove_subtree(&ctx->ws, full_path);
            ctx_delete_subtree(ctx, full_path);
        } else {
            ctx_delete(ctx, full_path);
        }
    }

    if (event->mask & IN_ISDIR)
        return;
    if (event->mask & IN_OPEN)
        ctx_record_open(ctx, full_path);

    /* A write is a stronger sign of interest than a read. An editor's atomic save is a rename of a
     * temp file onto the path, which arrives as IN_MOVED_TO rather than IN_CLOSE_WRITE, so both
     * count; a file moved in from outside the tree counts as an edit too, which is fair enough.
     */
    if (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO))
        ctx_record_edit(ctx, full_path);
}

void scan_read_inotify(search_ctx *ctx)
{
    char buf[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    for (;;) {
        ssize_t len = read(ctx->ws.inotify_fd, buf, sizeof(buf));
        if (len == -1 && errno != EAGAIN) {
            perror("read");
            break;
        }
        if (len <= 0)
            break;

        const struct inotify_event *event;
        for (char *ptr = buf; ptr < buf + len;
             ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *)ptr;

            /* The kernel dropped events (wd is -1 here): what happened is unknowable, so the whole
             * tree is walked again and compared with the index.
             */
            if (event->mask & IN_Q_OVERFLOW) {
                scan_request_rescan(ctx);
                continue;
            }
            if (event->mask & IN_IGNORED) {
                watch_drop(&ctx->ws, event->wd);
                continue;
            }

            /* The path is copied out under the table's lock: a scan worker may rename the entry
             * (watch_add on a directory renamed during an overflow) at any moment after that.
             */
            char dir[PATH_MAX];
            if (watch_lookup(&ctx->ws, event->wd, dir, sizeof(dir)))
                continue;

            /* The watched directory itself went away. Below the root its parent's event already
             * covers it; the root has no watched parent, so this is the only notice, and with it
             * every path in the index stops meaning anything.
             */
            if (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_UNMOUNT)) {
                ctx_delete_subtree(ctx, dir);
                if (!strcmp(dir, ctx->root_path))
                    atomic_store(&ctx->root_lost, true);
                continue;
            }
            handle_event(ctx, event, dir);
        }
    }
}

/* The scan thread and its rescan channel */

void scan_request_rescan(search_ctx *ctx)
{
    pthread_mutex_lock(&ctx->scan_lock);
    ctx->rescan_requested = true;
    pthread_cond_signal(&ctx->scan_cond);
    pthread_mutex_unlock(&ctx->scan_lock);
}

/* Blocks until a rescan is requested or the program shuts down. Returns false for shutdown, which
 * wins over a pending request.
 */
static bool scan_wait_rescan(search_ctx *ctx)
{
    pthread_mutex_lock(&ctx->scan_lock);
    while (!ctx->rescan_requested && !ctx->shutdown)
        pthread_cond_wait(&ctx->scan_cond, &ctx->scan_lock);
    bool go = ctx->rescan_requested && !ctx->shutdown;
    ctx->rescan_requested = false;
    pthread_mutex_unlock(&ctx->scan_lock);
    return go;
}

/* One pass over the root: a diff against what the index already holds when diff is set (after a
 * snapshot load, or on every rescan: mark everything unseen, walk, sweep what was not seen), a
 * plain walk otherwise. A pass cut short by shutdown must not sweep: what it did not reach is not
 * gone. The caller reports the pass finished (ctx_scan_finished), since only it knows whether the
 * pass calls for a full PageRank computation.
 *
 * Returns whether the pass completed.
 */
bool scan_pass(search_ctx *ctx, bool diff)
{
    if (diff)
        ctx_mark_all_unseen(ctx);
    scan_submit(ctx, ctx->root_path);
    tp_wait(ctx->tp);

    bool completed = !ctx->shutdown;
    if (completed && diff)
        ctx_sweep_unseen(ctx);
    return completed;
}

/* The first pass indexes the root; the thread then sleeps until the inotify reader asks for a
 * rescan after a queue overflow, and runs the same diff pass against the live index.
 *
 * Only the first pass after a snapshot load asks for a full PageRank computation: the snapshot
 * has the scores but not the residuals, and the computation rebuilds both. Everywhere else the
 * state is already exact, since every change is applied incrementally: a fresh index has no opens
 * (a full computation would give zero everywhere), and a rescan changes no opens or edges.
 */
void *scan_thread(void *arg)
{
    search_ctx *ctx = (search_ctx *)arg;

    bool completed = scan_pass(ctx, ctx->loaded);
    ctx_scan_finished(ctx, completed && ctx->loaded);
    while (scan_wait_rescan(ctx)) {
        ctx_scan_started(ctx);
        scan_pass(ctx, true);
        ctx_scan_finished(ctx, false);
        watch_prune(&ctx->ws);   /* IN_IGNORED for directories removed during the overflow was lost too */
    }
    return NULL;
}
