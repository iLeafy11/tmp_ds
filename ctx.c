#include "ctx.h"
#include "watch.h"
#include "threadpool.h"
#include "path.h"

#include <sys/stat.h>
#include <math.h>
#include <time.h>

void index_entry_free(void *ptr)
{
    index_entry *entry = ptr;
    if (!entry)
        return;
    free(entry->path);
    free(entry);
}

/* Index operations
 *
 * An entry lives in three places that must agree: the path index (bsl), pr_entries[pr_id] for score
 * sync, and the token index. register_entry keeps the latter two in step with the first; all of
 * them run under ctx->lock held for writing. Entries are never taken out again while the program
 * runs, only marked dead (see ctx.h).
 */

/* Grows pr_entries so that id is a valid index. Returns the cell, or NULL on OOM. */
static index_entry **pr_entries_cell(search_ctx *ctx, int id)
{
    if (id >= ctx->pr_entry_cap) {
        int new_cap = round_up_pow2(id + 1);
        if (new_cap < 1024)
            new_cap = 1024;
        index_entry **tmp = realloc(ctx->pr_entries, new_cap * sizeof(*tmp));
        if (!tmp)
            return NULL;
        memset(tmp + ctx->pr_entry_cap, 0,
               (new_cap - ctx->pr_entry_cap) * sizeof(*tmp));
        ctx->pr_entries = tmp;
        ctx->pr_entry_cap = new_cap;
    }
    return &ctx->pr_entries[id];
}

static int register_entry(search_ctx *ctx, index_entry *entry)
{
    index_entry **cell = pr_entries_cell(ctx, entry->pr_id);
    if (!cell)
        return -1;
    *cell = entry;
    token_index_add(ctx->tokens, entry->path, entry);
    return 0;
}

static void index_changed(search_ctx *ctx)
{
    atomic_fetch_add(&ctx->index_gen, 1);
    atomic_fetch_add(&ctx->change_gen, 1);
}

static void mark_dead(search_ctx *ctx, index_entry *entry)
{
    if (entry->dead)
        return;
    entry->dead = true;
    ctx->n_live--;
    index_changed(ctx);
}

/* The file system confirms that entry's path exists: marks it seen (the diff scan after loading a
 * snapshot relies on this), revives it if it was dead, and refreshes its type, since a path can be
 * replaced by one of the other kind. The counterpart of mark_dead. Called with ctx->lock held for
 * writing.
 */
static void mark_alive(search_ctx *ctx, index_entry *entry, bool is_dir)
{
    entry->seen = true;
    if (entry->dead) {
        entry->dead = false;
        ctx->n_live++;
        index_changed(ctx);
    }
    if (entry->is_dir != is_dir) {
        entry->is_dir = is_dir;
        index_changed(ctx);
    }
}

/* Makes sure path is indexed. The two locks are never nested here: the graph node is taken first,
 * on its own, then the index is updated under ctx->lock. Nesting them (graph_lock inside ctx->lock)
 * once made every search wait behind an insert that was itself waiting for pr_loop to finish a full
 * computation; now only the inserting thread waits. The price is a re-lookup after the node is
 * taken, and, when the path was inserted meanwhile or the insert fails, a node that nobody points
 * to: harmless (v = 0, no edges) and dropped at the next save like any ghost.
 */
void ctx_insert(search_ctx *ctx, const char *path, bool is_dir)
{
    pthread_rwlock_wrlock(&ctx->lock);
    index_entry *entry = bsl_lookup(ctx->index, path);
    if (entry)
        mark_alive(ctx, entry, is_dir);
    pthread_rwlock_unlock(&ctx->lock);
    if (entry)
        return;

    entry = calloc(1, sizeof(*entry));
    if (!entry)
        return;
    entry->path = strdup(path);
    if (!entry->path) {
        free(entry);
        return;
    }
    entry->is_dir = is_dir;
    entry->seen = true;

    /* A new node has v = 0: never opened, no score, and no effect on anyone else's. The graph node
     * is the entry's identity for scoring, so an entry that cannot get one is not kept.
     */
    pthread_rwlock_wrlock(&ctx->graph_lock);
    entry->pr_id = pr_add_node(ctx->graph);
    pthread_rwlock_unlock(&ctx->graph_lock);
    if (entry->pr_id < 0) {
        index_entry_free(entry);
        return;
    }

    pthread_rwlock_wrlock(&ctx->lock);
    index_entry *existing = bsl_lookup(ctx->index, path);
    if (existing) {                       /* someone else inserted it while we took the node */
        mark_alive(ctx, existing, is_dir);
        index_entry_free(entry);
    } else if (bsl_insert(ctx->index, path, entry)) {
        index_entry_free(entry);
    } else if (register_entry(ctx, entry)) {
        bsl_delete(ctx->index, path, index_entry_free);
    } else {
        ctx->n_live++;
        index_changed(ctx);
    }
    pthread_rwlock_unlock(&ctx->lock);
}

void ctx_delete(search_ctx *ctx, const char *path)
{
    pthread_rwlock_wrlock(&ctx->lock);
    index_entry *entry = bsl_lookup(ctx->index, path);
    if (entry)
        mark_dead(ctx, entry);
    pthread_rwlock_unlock(&ctx->lock);
}

static void mark_dead_cb(const char *key, void *slot, void *arg)
{
    (void)key;
    mark_dead(arg, slot);
}

int ctx_delete_subtree(search_ctx *ctx, const char *path)
{
    char prefix[PATH_MAX];
    if (path_join(prefix, sizeof(prefix), path, "") < 0)
        return 0;
    pthread_rwlock_wrlock(&ctx->lock);
    int before = ctx->n_live;
    index_entry *entry = bsl_lookup(ctx->index, path);
    if (entry)
        mark_dead(ctx, entry);
    bsl_prefix_for_each(ctx->index, prefix, mark_dead_cb, ctx);
    int n = before - ctx->n_live;
    pthread_rwlock_unlock(&ctx->lock);
    return n;
}

/* PageRank access recording */

/* Caller must hold q->lock. Returns &q->dirty_index[id], growing the table if needed; NULL on
 * OOM.
 */
static int *dirty_index_cell(pr_mailbox *q, int id)
{
    if (id >= q->dirty_index_cap) {
        int new_cap = round_up_pow2(id + 1);
        if (new_cap < 1024)
            new_cap = 1024;

        int *tmp = realloc(q->dirty_index, new_cap * sizeof(int));
        if (!tmp)
            return NULL;
        for (int i = q->dirty_index_cap; i < new_cap; i++)
            tmp[i] = -1;
        q->dirty_index = tmp;
        q->dirty_index_cap = new_cap;
    }
    return &q->dirty_index[id];
}

/* Queues amount for pr_loop: onto from's open count when to is NULL (an open or an edit of from),
 * onto the edge from -> to otherwise (a transition). Updates for one node coalesce into a single
 * pr_node_update, found through dirty_index, so a node opened many times between two batches is
 * one entry in dirty[].
 */
static void record_update(search_ctx *ctx, pr_node *from, pr_node *to, double amount)
{
    pr_mailbox *q = &ctx->pr_mail;

    pthread_mutex_lock(&q->lock);

    int *cell = dirty_index_cell(q, from->id);
    if (!cell) {
        pthread_mutex_unlock(&q->lock);
        return;
    }

    pr_node_update *update = *cell >= 0 ? &q->dirty[*cell] : NULL;
    if (!update) {
        if (q->n >= q->cap) {
            int new_cap = q->cap ? q->cap << 1 : 16;
            pr_node_update *tmp = realloc(q->dirty, new_cap * sizeof(*tmp));
            if (!tmp)
                goto out;
            q->dirty = tmp;
            q->cap = new_cap;
        }
        *cell = q->n;
        update = &q->dirty[q->n++];
        *update = (pr_node_update){.from = from};
    }

    if (!to) {                    /* an open of `from` itself, no edge */
        update->opens += amount;
        goto out;
    }

    for (int i = 0; i < update->n; i++) {
        if (update->deltas[i].to == to) {
            update->deltas[i].amount += amount;
            goto out;
        }
    }

    if (update->n >= update->cap) {
        int new_cap = update->cap ? update->cap << 1 : 4;
        pr_edge_delta *tmp = realloc(update->deltas, new_cap * sizeof(*tmp));
        if (!tmp)
            goto out;
        update->deltas = tmp;
        update->cap = new_cap;
    }
    update->deltas[update->n++] = (pr_edge_delta){.to = to, .amount = amount};

out:
    pthread_cond_signal(&q->dirty_cond);
    pthread_mutex_unlock(&q->lock);
}

/* True if key was already counted within the window; records it otherwise. Event loop only. */
static bool repeat_seen(repeat_window *w, uint64_t key, double now)
{
    for (int i = 0; i < 8; i++) {
        if (w->slot[i].at && w->slot[i].key == key &&
            now - w->slot[i].at < ACCESS_REPEAT_WINDOW_SECS)
            return true;
    }
    w->slot[w->next] = (typeof(w->slot[0])){key, now};
    w->next = (w->next + 1) & 7;
    return false;
}

static uint64_t edge_key(int from, int to)
{
    return (uint64_t)(uint32_t)from << 32 | (uint32_t)to;
}

/* Looks the path up and passes the timing gate; the pr_id, or -1 if the event is not the user's. */
static int gated_access(search_ctx *ctx, const char *path, double now, double *gap_out)
{
    pthread_rwlock_rdlock(&ctx->lock);
    index_entry *entry = bsl_lookup(ctx->index, path);
    int id = entry && !entry->dead ? entry->pr_id : -1;
    pthread_rwlock_unlock(&ctx->lock);
    if (id < 0)
        return -1;

    /* Timing gate. A burst of events at program speed is not the user; the burst's own events still
     * move last_access_at, so the whole burst stays inside the gate, but the human's last file is
     * kept as the from side for the next real open.
     */
    double gap = ctx->last_access_at > 0 ? now - ctx->last_access_at : ACCESS_MAX_GAP_SECS + 1;
    ctx->last_access_at = now;
    if (gap < ACCESS_MIN_GAP_SECS)
        return -1;
    *gap_out = gap;
    return id;
}

/* The clock of the timing gate and the repeat windows: monotonic, since they measure intervals
 * within one run and nothing of them is saved. The wall clock jumps when NTP or a person sets it; a
 * step back made the next gap negative, and the open was taken for program speed. Decay is the
 * other clock and stays on wall time, because it has to span restarts.
 */
static double access_clock(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

void ctx_record_open_at(search_ctx *ctx, const char *path, double now)
{
    double gap;
    int accessed_pr_id = gated_access(ctx, path, now, &gap);
    if (accessed_pr_id < 0)
        return;

    int last_pr_id = gap > ACCESS_MAX_GAP_SECS ? -1 : ctx->last_accessed_pr_id;
    ctx->last_accessed_pr_id = accessed_pr_id;

    /* pr_add_node may realloc the block pointer array; pr_node itself never moves, so the pointers
     * remain valid after graph_lock is released.
     */
    pthread_rwlock_rdlock(&ctx->graph_lock);
    pr_node *to = pr_get(ctx->graph, accessed_pr_id);
    pr_node *from = last_pr_id >= 0 ? pr_get(ctx->graph, last_pr_id) : NULL;
    pthread_rwlock_unlock(&ctx->graph_lock);

    bool recorded = false;
    if (!repeat_seen(&ctx->repeat_opens, (uint64_t)accessed_pr_id, now)) {
        record_update(ctx, to, NULL, 1.0);            /* the open itself */
        recorded = true;
    }
    if (from && from != to && !repeat_seen(&ctx->repeat_edges, edge_key(last_pr_id, accessed_pr_id), now)) {
        record_update(ctx, from, to, 1.0);            /* the transition */
        recorded = true;
    }
    if (recorded)
        atomic_fetch_add(&ctx->change_gen, 1);   /* something the snapshot should carry */
}

void ctx_record_open(search_ctx *ctx, const char *path)
{
    ctx_record_open_at(ctx, path, access_clock());
}

void ctx_record_edit_at(search_ctx *ctx, const char *path, double now)
{
    double gap;
    int id = gated_access(ctx, path, now, &gap);
    if (id < 0 || repeat_seen(&ctx->repeat_edits, (uint64_t)id, now))
        return;
    pthread_rwlock_rdlock(&ctx->graph_lock);
    pr_node *node = pr_get(ctx->graph, id);
    pthread_rwlock_unlock(&ctx->graph_lock);
    record_update(ctx, node, NULL, EDIT_WEIGHT);
    atomic_fetch_add(&ctx->change_gen, 1);
}

void ctx_record_edit(search_ctx *ctx, const char *path)
{
    ctx_record_edit_at(ctx, path, access_clock());
}

/* The index lock for reading is enough: it keeps pr_entries from moving, and the stores are atomic.
 * A search running meanwhile may collect some candidates before a store and some after, which only
 * means a ranking half a batch old; its sort is safe either way, because topk copies each score
 * once, when it sees the candidate, and compares the copies. (This took the lock for writing while
 * the comparator still read the live scores, which a store mid-sort would have made inconsistent.)
 */
void ctx_sync_scores(search_ctx *ctx)
{
    pthread_rwlock_rdlock(&ctx->lock);
    pthread_rwlock_rdlock(&ctx->graph_lock);
    int n = ctx->graph->n < ctx->pr_entry_cap ? ctx->graph->n
                                             : ctx->pr_entry_cap;
    for (int i = 0; i < n; i++) {
        if (ctx->pr_entries[i])
            atomic_store(&ctx->pr_entries[i]->pgrk,
                         atomic_load(&pr_get(ctx->graph, i)->p));
    }
    pthread_rwlock_unlock(&ctx->graph_lock);
    pthread_rwlock_unlock(&ctx->lock);
    atomic_fetch_add(&ctx->score_gen, 1);
    atomic_fetch_add(&ctx->change_gen, 1);
}

bool ctx_take_pr_batch(search_ctx *ctx, pr_node_update **dirty, int *n_dirty, bool *full)
{
    pr_mailbox *q = &ctx->pr_mail;

    pthread_mutex_lock(&q->lock);
    while (!q->n && !q->full_recompute && !(ctx->shutdown && q->scan_done))
        pthread_cond_wait(&q->dirty_cond, &q->lock);

    if (!q->n && !q->full_recompute) {   /* woken only because we are shutting down */
        pthread_mutex_unlock(&q->lock);
        return false;
    }

    *full = q->full_recompute;
    q->full_recompute = false;

    *dirty = q->dirty;
    *n_dirty = q->n;
    for (int i = 0; i < q->n; i++)
        q->dirty_index[q->dirty[i].from->id] = -1;
    q->dirty = NULL;
    q->n = 0;
    q->cap = 0;

    pthread_mutex_unlock(&q->lock);
    return true;
}

void ctx_free_pr_batch(pr_node_update *dirty, int n_dirty)
{
    for (int i = 0; i < n_dirty; i++)
        free(dirty[i].deltas);
    free(dirty);
}

void ctx_scan_started(search_ctx *ctx)
{
    pthread_mutex_lock(&ctx->pr_mail.lock);
    ctx->pr_mail.scan_done = false;
    pthread_mutex_unlock(&ctx->pr_mail.lock);
}

void ctx_scan_finished(search_ctx *ctx, bool request_full)
{
    pr_mailbox *q = &ctx->pr_mail;

    pthread_mutex_lock(&q->lock);
    q->scan_done = true;
    if (request_full)
        q->full_recompute = true;
    pthread_cond_signal(&q->dirty_cond);
    pthread_mutex_unlock(&q->lock);
}

/* Lifecycle */

search_ctx *ctx_create(const char *root_path)
{
    search_ctx *ctx = calloc(1, sizeof(search_ctx));
    if (!ctx)
        return NULL;

    /* Everything that cannot fail goes first, so that any later failure can simply hand the
     * partially built context to ctx_destroy. Fields left zero by calloc (pr_mail counters,
     * pointers, shutdown) are already in their initial state.
     */
    ctx->epoll_fd = -1;
    ctx->shutdown_fd = -1;
    ctx->signal_fd = -1;
    ctx->listen_fd = -1;
    ctx->timer_fd = -1;
    ctx->last_accessed_pr_id = -1;
    pthread_rwlock_init(&ctx->lock, NULL);
    pthread_rwlock_init(&ctx->graph_lock, NULL);
    pthread_mutex_init(&ctx->pr_mail.lock, NULL);
    pthread_cond_init(&ctx->pr_mail.dirty_cond, NULL);
    pthread_mutex_init(&ctx->scan_lock, NULL);
    pthread_cond_init(&ctx->scan_cond, NULL);

    if (watch_init(&ctx->ws) == -1)
        goto fail;

    ctx->index = bsl_create();
    if (!ctx->index)
        goto fail;

    ctx->tokens = token_index_create();
    if (!ctx->tokens)
        goto fail;

    ctx->graph = pr_create(1024);
    if (!ctx->graph)
        goto fail;

    ctx->root_path = path_strdup_normalized(root_path);
    if (!ctx->root_path)
        goto fail;
    /* Left zero (calloc) when the root cannot be stat'ed: a made-up root in a test. */
    path_identity(ctx->root_path, &ctx->root_dev, &ctx->root_ino, &ctx->root_btime);

    ctx->n_workers = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ctx->n_workers < 2)
        ctx->n_workers = 2;
    ctx->tp = tp_create(ctx->n_workers);
    if (!ctx->tp)
        goto fail;

    return ctx;

fail:
    ctx_destroy(ctx);
    return NULL;
}

void ctx_destroy(search_ctx *ctx)
{
    if (!ctx)
        return;

    watch_destroy(&ctx->ws);   /* closes the inotify fd too */

    if (ctx->tp)
        tp_destroy(ctx->tp);
    token_index_destroy(ctx->tokens);
    bsl_destroy(ctx->index, index_entry_free);
    pr_destroy(ctx->graph);
    pthread_rwlock_destroy(&ctx->lock);
    pthread_rwlock_destroy(&ctx->graph_lock);
    pthread_mutex_destroy(&ctx->pr_mail.lock);
    pthread_mutex_destroy(&ctx->scan_lock);
    pthread_cond_destroy(&ctx->scan_cond);
    pthread_cond_destroy(&ctx->pr_mail.dirty_cond);
    ctx_free_pr_batch(ctx->pr_mail.dirty, ctx->pr_mail.n);
    free(ctx->pr_mail.dirty_index);
    free(ctx->pr_entries);
    free(ctx->root_path);
    free(ctx);
}

bool ctx_decay(search_ctx *ctx, time_t now)
{
    if (ctx->half_life_secs <= 0 || now - ctx->last_decay < DECAY_INTERVAL_SECS)
        return false;
    double factor = pow(0.5, (double)(now - ctx->last_decay) / ctx->half_life_secs);
    pthread_rwlock_wrlock(&ctx->graph_lock);
    pr_decay(ctx->graph, factor);
    pthread_rwlock_unlock(&ctx->graph_lock);
    ctx->last_decay = now;
    ctx_sync_scores(ctx);                       /* p was scaled: show it (bumps change_gen too) */
    return true;
}

void ctx_forget(search_ctx *ctx)
{
    pthread_rwlock_wrlock(&ctx->lock);
    pthread_rwlock_wrlock(&ctx->graph_lock);
    pr_clear(ctx->graph);
    for (int i = 0; i < ctx->pr_entry_cap; i++)
        if (ctx->pr_entries[i])
            atomic_store(&ctx->pr_entries[i]->pgrk, 0.0);
    pthread_rwlock_unlock(&ctx->graph_lock);
    pthread_rwlock_unlock(&ctx->lock);

    pr_mailbox *q = &ctx->pr_mail;
    pthread_mutex_lock(&q->lock);
    for (int i = 0; i < q->n; i++)
        q->dirty_index[q->dirty[i].from->id] = -1;
    ctx_free_pr_batch(q->dirty, q->n);
    q->dirty = NULL;
    q->n = 0;
    q->cap = 0;
    pthread_mutex_unlock(&q->lock);

    ctx->last_accessed_pr_id = -1;
    ctx->last_access_at = 0;
    ctx->repeat_edges = ctx->repeat_opens = ctx->repeat_edits = (repeat_window){0};

    atomic_fetch_add(&ctx->score_gen, 1);
    atomic_fetch_add(&ctx->change_gen, 1);
}

/* Diff scan */

static void mark_unseen_cb(const char *key, void *slot, void *arg)
{
    (void)key;
    (void)arg;
    ((index_entry *)slot)->seen = false;
}

void ctx_mark_all_unseen(search_ctx *ctx)
{
    pthread_rwlock_wrlock(&ctx->lock);
    bsl_for_each(ctx->index, mark_unseen_cb, NULL);
    pthread_rwlock_unlock(&ctx->lock);
}

static void mark_seen_cb(const char *key, void *slot, void *arg)
{
    (void)key;
    (void)arg;
    ((index_entry *)slot)->seen = true;
}

void ctx_mark_subtree_seen(search_ctx *ctx, const char *path)
{
    char *dir = path_strdup_normalized(path);   /* keys carry no trailing slash */
    if (!dir)
        return;
    char prefix[PATH_MAX];
    if (path_join(prefix, sizeof(prefix), dir, "") >= 0) {
        pthread_rwlock_wrlock(&ctx->lock);
        index_entry *entry = bsl_lookup(ctx->index, dir);
        if (entry)
            entry->seen = true;
        bsl_prefix_for_each(ctx->index, prefix, mark_seen_cb, NULL);
        pthread_rwlock_unlock(&ctx->lock);
    }
    free(dir);
}

static void sweep_unseen_cb(const char *key, void *slot, void *arg)
{
    (void)key;
    index_entry *entry = slot;
    if (!entry->seen)
        mark_dead(arg, entry);
}

int ctx_sweep_unseen(search_ctx *ctx)
{
    pthread_rwlock_wrlock(&ctx->lock);
    int before = ctx->n_live;
    bsl_for_each(ctx->index, sweep_unseen_cb, ctx);
    int removed = before - ctx->n_live;
    pthread_rwlock_unlock(&ctx->lock);
    return removed;
}

/* Status */

void ctx_get_status(search_ctx *ctx, ctx_status *out)
{
    pthread_rwlock_rdlock(&ctx->lock);
    out->indexed = ctx->n_live;
    pthread_rwlock_unlock(&ctx->lock);

    pthread_mutex_lock(&ctx->pr_mail.lock);
    out->scan_done = ctx->pr_mail.scan_done;
    pthread_mutex_unlock(&ctx->pr_mail.lock);

    out->unwatched = atomic_load(&ctx->unwatched);
    out->root_lost = atomic_load(&ctx->root_lost);
    out->index_gen = atomic_load(&ctx->index_gen);
    out->score_gen = atomic_load(&ctx->score_gen);
}

