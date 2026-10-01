#ifndef _SEARCH_H
#define _SEARCH_H

#include "ctx.h"

enum { SEARCH_TOP_K = 20 };

typedef struct search_hit {
    char path[PATH_MAX];
    double score;
    bool is_dir;
} search_hit;

/* The one entry point for a query string, shared by the TUI, --query and any future client: empty
 * lists the root's direct children; a single byte or anything containing '/' is a path prefix,
 * relative to the root unless absolute; everything else is a token (substring) search.
 *
 * Returns the number of live matches, of which at most SEARCH_TOP_K are copied out.
 */
int search_query(search_ctx *ctx, const char *text, search_hit *hits, int *nhits);

#endif
