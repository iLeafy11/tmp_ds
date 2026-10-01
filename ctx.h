#ifndef _CTX_H
#define _CTX_H

#define _GNU_SOURCE
#define _XOPEN_SOURCE 500

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "list.h"
#include "bsl.h"
#include "pagerank.h"
#include "token_index.h"
#include "watch.h"

/* Per-file index entry stored as bsl slot. */
typedef struct index_entry {
    char *path;
    _Atomic double pgrk;    /* the node's p as last synced (ctx_sync_scores); searches sort by it */
    int pr_id;              /* index into PageRank graph; always valid for an entry in the index */
    bool is_dir;
    bool seen;
    bool dead;              /* deleted: kept in every structure, skipped by search and save */
} index_entry;

/* The keys counted in the last ACCESS_REPEAT_WINDOW_SECS, with the time each was counted: a
 * repeat within the window is not counted again. Eight slots, overwritten round-robin (bouncing
 * involves two or three files, not eight); it is never popped like a queue. A key is a pr_id, or
 * for a transition both ids packed into one word (edge_key).
 */
typedef struct repeat_window {
    struct { uint64_t key; double at; } slot[8];
    int next;
} repeat_window;

/* Everything pr_loop reads its instructions from: the pending edge-weight updates (producer-
 * consumer with take-all batch semantics), the request for a full computation, and the scan state
 * it waits on before it may exit. All fields are protected by lock. dirty_index[node id] is the
 * node's index into dirty[] (-1 if not queued) so that repeated accesses from the same node
 * coalesce into one pr_node_update.
 */
typedef struct pr_mailbox {
    pr_node_update *dirty;
    int n;
    int cap;
    int *dirty_index;
    int dirty_index_cap;
    bool full_recompute;
    bool scan_done;
    pthread_mutex_t lock;
    pthread_cond_t dirty_cond;
} pr_mailbox;

/* All shared state for one desktop search instance. Grouped by who owns what; the rule for each
 * block is in its comment. Two rwlocks in a fixed order, lock before graph_lock; everything else is
 * either an atomic, a single-thread field, or has a mutex of its own.
 */
typedef struct search_ctx {
    /* Identity and configuration. Set once at startup, read-only afterwards. */
    char *root_path;           /* root directory being indexed, absolute */
    /* The root's identity on disk, which a rename keeps and a deletion plus recreation does not
     * (the inode number can be reused, the birth time cannot): what a snapshot of this root
     * carries, so that a renamed root finds its history again. Zero when the root could not be
     * stat'ed (tests with made-up roots).
     */
    uint64_t root_dev, root_ino;
    int64_t root_btime;        /* nanoseconds; 0 when the filesystem reports no birth time */
    char snapshot_path[PATH_MAX]; /* the snapshot file, <snapshot_dir>/<hash>.bin */
    struct threadpool *tp;     /* scan pool; PageRank keeps its own */
    int n_workers;             /* pool size, also used for the PageRank pool */
    bool loaded;               /* a snapshot was loaded at startup (the first scan is a diff) */

    /* The index, under lock. Writers (insert, delete, sweep, load) take it for writing; searches,
     * save and ctx_sync_scores for reading (scores are atomics, and a search sorts copies of them).
     */
    pthread_rwlock_t lock;
    bsl *index;                /* path → index_entry, ordered */
    token_index *tokens;       /* suffix → posting list of entries */
    index_entry **pr_entries;  /* pr_entries[pr_id] → entry, for score sync and save */
    int pr_entry_cap;
    int n_live;                /* entries not marked dead; what the UI reports as indexed */

    /* The access graph, under graph_lock. pr_loop applies edge updates and computes under it;
     * ctx_insert adds nodes under it; save reads under it. Scores travel to entries as atomics.
     */
    pthread_rwlock_t graph_lock;
    pr_graph *graph;           /* nodes never move or go away while running; compacted on save */
    pr_mailbox pr_mail;        /* what pr_loop acts on: edge updates, full-recompute requests; its own mutex */
    int last_accessed_pr_id;   /* previous counted IN_OPEN, the from side of the next edge; event loop only */
    double last_access_at;     /* previous IN_OPEN of any kind, monotonic seconds; 0 = none yet */
    double half_life_secs;     /* edge-weight half-life; 0 = no decay. Set once at startup */
    time_t last_decay;         /* when the weights were last decayed; persisted so downtime counts */

    /* One repeat window each for transitions, opens and edits, so that bouncing between two files
     * (editor and terminal) or an editor re-reading a file counts once per
     * ACCESS_REPEAT_WINDOW_SECS rather than once per event. Event loop only.
     */
    repeat_window repeat_edges, repeat_opens, repeat_edits;

    /* Generations. Atomics with no lock: readers compare, writers bump.
     *   index_gen   any change to index contents (insert, delete, type), so a UI can tell a
     *               delete paired with an insert apart from no change
     *   score_gen   ctx_sync_scores ran
     *   change_gen  anything a snapshot would capture: the two above plus a new edge
     *   saved_gen   change_gen at the last save; event loop only, so no atomic
     */
    _Atomic unsigned index_gen;
    _Atomic unsigned score_gen;
    _Atomic unsigned change_gen;
    unsigned saved_gen;

    /* The event loop's descriptors, all in epoll_fd. Created by event_init and closed by
     * event_close (run.c); -1 when absent. shutdown is the flag every thread polls to leave.
     */
    int epoll_fd;
    watch_state ws;            /* inotify fd plus the wd → path table, its own mutex */
    int shutdown_fd;           /* eventfd: event_wake wakes the loop */
    int signal_fd;             /* signalfd: SIGINT/SIGTERM/SIGHUP arrive here, not as handlers */
    int listen_fd;             /* daemon mode: the Unix socket clients connect to */
    int timer_fd;              /* periodic snapshot tick */
    _Atomic bool shutdown;

    /* The scan thread and watch health. After the first pass the scan thread sleeps on scan_cond;
     * an inotify queue overflow requests a rescan (mark unseen, walk, sweep), requests during a
     * pass coalesce. unwatched and root_lost are shown in the UI label.
     */
    pthread_mutex_t scan_lock;
    pthread_cond_t scan_cond;
    bool rescan_requested;
    _Atomic int unwatched;     /* directories whose inotify watch could not be added */
    _Atomic bool root_lost;    /* the root itself was deleted, moved away or unmounted */
} search_ctx;

void index_entry_free(void *ptr);   /* bsl free_slot callback; NULL-safe */

search_ctx *ctx_create(const char *root_path);
void ctx_destroy(search_ctx *ctx);

/* Deletion only marks. A dead entry stays in the path index, the token index and the graph: search
 * and save skip it, and inserting its path again revives it with its graph node and score intact
 * (an editor's delete-and-rewrite, or a directory removed and recreated). Marking is O(subtree)
 * with no token index work, cheap enough to run inline in the event loop so that a delete and a
 * later recreate of the same path are applied in event order. Dead entries are dropped when the
 * snapshot is written and never loaded back.
 */
void ctx_insert(search_ctx *ctx, const char *path, bool is_dir);   /* idempotent */
void ctx_delete(search_ctx *ctx, const char *path);

/* A directory and everything below it, under one write lock. Returns how many live entries died. */
int ctx_delete_subtree(search_ctx *ctx, const char *path);

#define ACCESS_REPEAT_WINDOW_SECS 60

/* What a human transition looks like in time. Opens closer together than the minimum are a program
 * walking files (grep -r, thumbnails, a build): neither an open nor an edge is counted, and the
 * human's last file stays the from side. Opens further apart than the maximum are two sessions: the
 * open counts, the edge is not made. The latter is also why last_accessed_pr_id is not persisted:
 * yesterday's last file and today's first are not a transition.
 */
#define ACCESS_MIN_GAP_SECS 0.5
#define ACCESS_MAX_GAP_SECS 600.0

/* A file written to is worth this much more than one only read: an edit adds EDIT_WEIGHT to the
 * file's v on top of the open that preceded it. Same timing gate and repeat window as opens (one
 * edit per file per window); no edge, an edit is not a transition.
 */
#define EDIT_WEIGHT 1.0

/* PageRank update queue. Producers: ctx_record_open and ctx_record_edit (file opens and writes, on
 * the event loop) and ctx_scan_finished. The single consumer, pr_loop, blocks in ctx_take_pr_batch.
 */
void ctx_record_open(search_ctx *ctx, const char *path);
void ctx_record_open_at(search_ctx *ctx, const char *path, double now);   /* monotonic seconds; for tests */
void ctx_record_edit(search_ctx *ctx, const char *path);
void ctx_record_edit_at(search_ctx *ctx, const char *path, double now);
void ctx_sync_scores(search_ctx *ctx);

/* Waits for work, then hands over every pending update (free with ctx_free_pr_batch) and whether a
 * full recomputation was requested.
 *
 * Returns false once the program is shutting down and nothing is left to compute.
 */
bool ctx_take_pr_batch(search_ctx *ctx, pr_node_update **dirty, int *n_dirty, bool *full);
void ctx_free_pr_batch(pr_node_update *dirty, int n_dirty);

/* The initial scan is over (or was abandoned on shutdown). request_full schedules a full PageRank
 * recomputation over the now complete graph.
 */
void ctx_scan_finished(search_ctx *ctx, bool request_full);

/* A rescan pass began: scan_done flips back off so the UI shows it. */
void ctx_scan_started(search_ctx *ctx);


/* Diff scan after loading a snapshot: mark everything unseen, let the scan mark what still
 * exists, then sweep the rest. Both lock internally.
 */
void ctx_mark_all_unseen(search_ctx *ctx);
int ctx_sweep_unseen(search_ctx *ctx);


/* Marks path and everything below it seen: the scan could not list the directory (permissions, I/O
 * error, no worker), so the sweep must not take its cached entries for deleted ones.
 */
void ctx_mark_subtree_seen(search_ctx *ctx, const char *path);

/* The index status a UI shows next to its results, taken in one go: how many live entries, whether
 * the scan is over, the watch health, and two generation numbers a client compares to decide
 * whether its results are stale (score_gen moves when PageRank scores were synced, index_gen when
 * the index contents changed; a size comparison would miss a delete paired with an insert).
 */
typedef struct ctx_status {
    int indexed;
    bool scan_done;
    int unwatched;             /* directories whose inotify watch could not be added */
    bool root_lost;            /* the root itself was deleted, moved away or unmounted */
    unsigned index_gen;
    unsigned score_gen;
} ctx_status;

void ctx_get_status(search_ctx *ctx, ctx_status *out);

/* Forgets the access history and nothing else: every score, open count and edge, the updates
 * waiting for pr_loop, and the event loop's memory of the last open. The index stays, so the next
 * start is still a diff scan. Bumps score_gen (the UI redraws) and change_gen (the next snapshot
 * writes it out). Event loop thread, the only writer of the access state, or a process with no
 * threads yet. A batch pr_loop has already taken but not yet applied still lands afterwards: at
 * most one batch of history survives a clear that races with it.
 */
void ctx_forget(search_ctx *ctx);

/* Decays every edge weight, open count and score for the time elapsed since last_decay (nothing if
 * less than DECAY_INTERVAL_SECS has passed, or if decay is off), and syncs the scaled scores to the
 * entries.
 *
 * Returns whether it did. Also applied by snapshot_load to the loaded graph, so time the daemon was
 * not running counts too.
 */
#define DECAY_INTERVAL_SECS 3600
bool ctx_decay(search_ctx *ctx, time_t now);
#endif /* _CTX_H */
