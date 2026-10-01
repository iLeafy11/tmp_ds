#ifndef _TOKEN_INDEX_H
#define _TOKEN_INDEX_H

#include <stdbool.h>
#include <string.h>

typedef struct token_index token_index;

/* What the tokens are cut from and at: the basename of a path, split at these separators. The search
 * splits a query at the same characters to choose its probe, so the two must never disagree.
 */
static inline bool token_is_boundary(char c)
{
    return c == '.' || c == '-' || c == '_' || c == ' ';
}

static inline const char *token_basename(const char *path)
{
    const char *base = strrchr(path, '/');
    return base ? base + 1 : path;
}

/* Shortest token: one CJK character (three bytes) is a token, one ASCII letter is not. */
#define TOKEN_MIN_BYTES 2

token_index *token_index_create(void);
void token_index_destroy(token_index *ti);

/* Every suffix of path's basename gets entry appended to its posting list. A suffix produced twice
 * by one basename ("ab.ab") is deduplicated per call, so an entry sits in a list at most once.
 * There is no removal: a deleted entry stays in its lists marked dead and is filtered by the caller
 * of search; the index is rebuilt from the surviving paths at the next load.
 */
void token_index_add(token_index *ti, const char *path, void *entry);

/* Prefix search on token suffixes. Calls cb once per unique matching entry (deduplicated by entry
 * pointer).
 *
 * Returns total unique match count.
 */
int token_index_search(token_index *ti, const char *query,
                       void (*cb)(void *entry, void *arg), void *arg);

/* Number of (suffix, entry) pairs over all posting lists. */
int token_index_count(token_index *ti);

/* Number of (token, entry) pairs a prefix search for prefix would visit, dead entries and a file
 * reached through two of its suffixes both counted, or the first partial sum above limit: the walk
 * stops as soon as the count exceeds limit, so comparing candidates against a known minimum costs
 * a few cursor steps. A limit below 0 means no limit.
 */
int token_index_prefix_postings(token_index *ti, const char *prefix, int limit);

#endif
