#ifndef SERVER_H
#define SERVER_H

#include "ctx.h"

/* Unix domain socket front door of the daemon. One request per connection: the client sends one
 * line, gets one JSON line back, and the connection is closed. A request is a verb, then for some
 * verbs a space and an argument:
 *   query <text>   run text through search_query; the reply below
 *   status         the reply below with no hits: what a UI polls to learn whether anything moved
 *   clear          forget the access history (scores, opens, edges), keep the index, save;
 *                  replies {"ok":true}
 *   stop           save and exit; replies {"ok":true} first
 * Anything else gets {"error":"..."}. The reply line itself is defined in reply.h.
 */
/* Creates the listening socket at path (non-blocking, close-on-exec). The daemon's claim on the
 * path is a flock on "<path>.lock", held until server_close: a second instance fails to lock and
 * must not start, and only the holder removes a stale socket file before binding, so two instances
 * starting at once cannot unlink each other's socket. The lock file itself is never removed (that
 * would reopen the race).
 *
 * Returns the fd, or -1 with the reason on stderr.
 */
int server_listen(const char *path);

/* Serves one accepted connection synchronously and closes it.
 *
 * Returns 0, or -1 if the request could not be read or answered. A stop request sets ctx->shutdown;
 * the event loop notices and ends.
 */
int server_serve_one(search_ctx *ctx, int client_fd);

void server_close(int listen_fd, const char *path);

#endif /* SERVER_H */
