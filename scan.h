#ifndef _SCAN_H
#define _SCAN_H

#include "ctx.h"

/* Keeps the index in step with the file system: the walk of the tree (on the pool), and the inotify
 * events that report changes afterwards.
 */

/* Queues a walk of path and everything below it on the scan pool. */
int scan_submit(search_ctx *ctx, const char *path);

/* One pass over the whole root, waiting for the pool to finish; see scan.c. Returns whether it
 * completed (shutdown cuts a pass short).
 */
bool scan_pass(search_ctx *ctx, bool diff);

/* Thread entry, arg is the search_ctx: the first pass, then a diff pass whenever a rescan is
 * requested, until shutdown.
 */
void *scan_thread(void *arg);

/* Asks the scan thread for a diff pass (the inotify queue overflowed), or wakes it to see shutdown.
 * Requests during a pass coalesce into one.
 */
void scan_request_rescan(search_ctx *ctx);

/* Drains the inotify fd and applies every event to the index. Event loop thread. */
void scan_read_inotify(search_ctx *ctx);

#endif
