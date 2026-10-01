#ifndef _REPLY_H
#define _REPLY_H

#include "ctx.h"
#include "search.h"

#include <stdio.h>

/* The reply line, the one format between the index and whoever shows its results: the socket
 * (server.c writes it, client.c reads it), --query --json, and the TUI (which holds the last one).
 * A query reply carries the hits and the index status the UI shows, so a client needs no second
 * request to draw its label:
 *   {"total":N,"root":"/abs/dir","indexed":N,"scan_done":bool,"unwatched":N,"root_lost":bool,
 *    "index_gen":N,"score_gen":N,"hits":[{"path":"...","score":F,"is_dir":bool},...]}
 * query_reply is that line as a struct.
 */
typedef struct query_reply {
    search_hit hits[SEARCH_TOP_K];
    int nhits;
    int total;                 /* live matches, of which nhits were copied out */
    char root[PATH_MAX];       /* the daemon's root, so a client can show root-relative paths */
    ctx_status status;         /* the six status members of the line, as ctx_get_status gives them */
} query_reply;

/* Runs the query and snapshots the status into out. */
void reply_fill(search_ctx *ctx, const char *text, query_reply *out);

/* Snapshots the status only: out has no hits. */
void reply_fill_status(search_ctx *ctx, query_reply *out);

/* Writes a reply as one JSON line. */
void reply_write(FILE *out, const query_reply *r);

/* Parses one reply line: exactly the shape reply_write produces, whitespace and any member order
 * tolerated, anything else rejected. Returns 0 or -1.
 */
int reply_parse(const char *json, query_reply *out);

#endif /* _REPLY_H */
