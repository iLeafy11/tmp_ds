#ifndef _SNAPSHOT_H
#define _SNAPSHOT_H

#include "ctx.h"

/* The snapshot: the index and the access graph written to one file, so that the next start begins
 * from it with a diff scan instead of an empty index. One file per root, outside the root, in
 * $XDG_STATE_HOME/desktop-search/snapshots; the format is described at the top of snapshot.c.
 */

/* Writes the snapshot to filepath (a temp file, fsync, rename): the previous one stays in place
 * until the new one is complete. Returns 0 or -1.
 */
int snapshot_save(search_ctx *ctx, const char *filepath);

/* Replaces the index and the graph with the snapshot's, after validating all of it; a file that
 * is missing, of another format or another root, or damaged anywhere leaves ctx as it was.
 * Returns 0 or -1.
 */
int snapshot_load(search_ctx *ctx, const char *filepath);

/* Saves if anything changed since the last save: what the snapshot tick (run.c) does after its
 * decay, and what a clear request does right away. Event loop thread.
 */
void snapshot_save_if_changed(search_ctx *ctx);

/* The directory that holds every snapshot: $XDG_STATE_HOME/desktop-search/snapshots. Created if
 * missing; snapshots still at their old place ($XDG_CACHE_HOME/desktop-search) are moved in first.
 */
bool snapshot_dir(char *dir, size_t size);

/* Sets ctx->snapshot_path to this root's snapshot, "<snapshot dir>/<FNV-1a of the root path>.bin",
 * and, when that file is missing, adopts the snapshot of the same directory under a former name
 * (found by the root's identity in the file headers, renamed into place). Returns 0, or -1 when no
 * snapshot directory can be had.
 */
int snapshot_locate(search_ctx *ctx);

#endif /* _SNAPSHOT_H */
