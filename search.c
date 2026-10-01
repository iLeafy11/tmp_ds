#include "search.h"
#include "path.h"

#include <stdlib.h>
#include <string.h>

/* Everything here only reads the index: the read side of ctx->lock, the two indexes, the graph size
 * for score display and the root for prefixes. Results are copied out under the lock, so a caller
 * never holds a pointer into the index.
 */

/* How well a hit's name matches the query, for token searches. A file whose name is exactly what
 * was typed goes above one that merely contains it, whatever their scores: the score orders within
 * a tier. Prefix and root listings have no notion of match quality; every hit is tier 0.
 */
enum match_tier {
    MATCH_EXACT,      /* the basename is the query */
    MATCH_PREFIX,     /* the basename starts with the query */
    MATCH_SEGMENT,    /* a segment (after '.', '-', '_', ' ') starts with the query */
    MATCH_INSIDE,     /* the query is somewhere else inside a segment */
};

/* A candidate carries the score it had when it was seen, so the comparator is stable for the whole
 * sort however the scores move meanwhile, and the displayed score is the one that placed the hit.
 */
typedef struct ranked {
    index_entry *entry;
    double score;
    enum match_tier tier;
} ranked;

static enum match_tier match_tier(const char *path, const char *query)
{
    const char *base = token_basename(path);
    size_t qlen = strlen(query);
    if (!strcmp(base, query))
        return MATCH_EXACT;
    if (!strncmp(base, query, qlen))
        return MATCH_PREFIX;
    for (const char *p = strstr(base, query); p; p = strstr(p + 1, query)) {
        if (p > base && token_is_boundary(p[-1]))
            return MATCH_SEGMENT;
    }
    return MATCH_INSIDE;
}

static int rank_cmp(const ranked *a, const ranked *b)
{
    if (a->tier != b->tier) return a->tier < b->tier ? -1 : 1;
    if (a->score > b->score) return -1;
    if (a->score < b->score) return 1;
    return strcmp(a->entry->path, b->entry->path);
}

static int rank_cmp_ptr(const void *a, const void *b)
{
    return rank_cmp(a, b);
}

struct topk_results {
    ranked entries[SEARCH_TOP_K];
    int n;
    int worst;      /* index of the lowest-ranked entry once the array is full */
    int total;      /* live entries seen, including those that did not make the top K */
    const char *query;   /* the token query, for match tiers; NULL for prefix and root listings */
};

static int topk_find_worst(const struct topk_results *top)
{
    int worst = 0;
    for (int i = 1; i < top->n; i++) {
        if (rank_cmp(&top->entries[i], &top->entries[worst]) > 0)
            worst = i;
    }
    return worst;
}

/* Once the array is full a candidate is compared with the current worst only; the worst is looked
 * up again just when a candidate displaces it. A root listing has thousands of candidates with
 * equal tier and score, where every comparison is a strcmp of two paths, so rescanning all K per
 * candidate was the whole cost of the search.
 */
static void topk_add(struct topk_results *top, index_entry *entry)
{
    ranked cand = {.entry = entry, .score = atomic_load(&entry->pgrk),
                   .tier = top->query ? match_tier(entry->path, top->query) : MATCH_EXACT};

    if (top->n < SEARCH_TOP_K) {
        top->entries[top->n++] = cand;
        if (top->n == SEARCH_TOP_K)
            top->worst = topk_find_worst(top);
        return;
    }

    if (rank_cmp(&cand, &top->entries[top->worst]) < 0) {
        top->entries[top->worst] = cand;
        top->worst = topk_find_worst(top);
    }
}

/* Dead entries are still in every structure; the searches filter them here and count the live. */
static void topk_add_live(struct topk_results *top, index_entry *entry)
{
    if (entry->dead)
        return;
    top->total++;
    topk_add(top, entry);
}

static void topk_from_index_cb(const char *key, void *slot, void *arg)
{
    (void)key;
    topk_add_live(arg, slot);
}

/* Token candidates are names containing the probe; only those containing the whole query count. */
static void topk_from_tokens_cb(void *entry, void *arg)
{
    struct topk_results *top = arg;
    index_entry *e = entry;

    if (strstr(token_basename(e->path), top->query))
        topk_add_live(top, e);
}

/* The same filter over every entry, for a query no probe can serve. */
static void topk_from_all_cb(const char *key, void *slot, void *arg)
{
    (void)key;
    topk_from_tokens_cb(slot, arg);
}

/* The token index holds suffixes of segments and nothing that crosses a boundary, so the query
 * "rank.c" alone finds nothing although pagerank.c should match. Each segment of the query is a
 * substring of one segment of every matching name, so a prefix search for any one segment finds
 * every match (plus names that contain only that segment, which the callback drops). The probe is
 * the segment whose prefix search visits the fewest candidates. The first segment is counted in
 * full; every later one stops counting as soon as it exceeds the best so far, which usually takes
 * one cursor step since the exact token heads its own range with the longest list. Ties go to the
 * longer segment, then the earlier one, so the count must strictly exceed the best to stop. A query
 * without any boundary is its own single segment.
 *
 * Returns NULL when no segment is token length (".c", "k.c", "a.b"): no token contains such a query
 * except a whole name starting with it, so the index cannot answer and the caller walks every
 * entry instead.
 */
static const char *choose_probe(token_index *ti, const char *query, char *buf, size_t size)
{
    const char *best = NULL;
    int best_len = 0, best_count = -1;   /* -1: no limit for the first segment counted */
    size_t qlen = strlen(query);

    for (size_t start = 0, i = 0; i <= qlen; i++) {
        if (i < qlen && !token_is_boundary(query[i]))
            continue;
        int len = (int)(i - start);
        if (len >= TOKEN_MIN_BYTES && (size_t)len < size) {
            memcpy(buf, query + start, (size_t)len);
            buf[len] = '\0';
            int count = token_index_prefix_postings(ti, buf, best_count);
            if (!best || count < best_count || (count == best_count && len > best_len)) {
                best = query + start;
                best_len = len;
                best_count = count;
            }
        }
        start = i + 1;
    }
    if (!best)
        return NULL;
    memcpy(buf, best, (size_t)best_len);
    buf[best_len] = '\0';
    return buf;
}

/* Sorts the collected entries and copies them out. Must be called with ctx->lock held, since the
 * entries are only guaranteed to stay alive under it.
 */
static void fill_hits(struct topk_results *top,
                      search_hit *hits, int *nhits)
{
    qsort(top->entries, top->n, sizeof(ranked), rank_cmp_ptr);
    for (int i = 0; i < top->n; i++) {
        index_entry *e = top->entries[i].entry;
        snprintf(hits[i].path, PATH_MAX, "%s", e->path);
        hits[i].score = PR_SCORE(top->entries[i].score);
        hits[i].is_dir = e->is_dir;
    }
    *nhits = top->n;
}

/* Lists the root directory's direct children. Keys are sorted, so a directory's whole subtree
 * ("<dir>/...") sits right behind the directory itself and is skipped with one seek to "<dir>0"
 * ('/' + 1); the cost is proportional to the number of children, not the size of the index.
 */
static int search_root(search_ctx *ctx, search_hit *hits, int *nhits)
{
    struct topk_results top = {0};
    char prefix[PATH_MAX];
    char after[PATH_MAX + 1];

    int prefix_len = path_join(prefix, sizeof(prefix), ctx->root_path, "");
    if (prefix_len < 0) {
        *nhits = 0;
        return 0;
    }

    /* Direct children only. A key with another '/' past the prefix lies inside some child's
     * subtree, so the cursor jumps to the first key after that subtree: child + ('/' + 1). Jumping
     * from the child's own entry instead would also skip siblings such as "sub.txt" or "sub-2",
     * which sort between "sub" and "sub/" because '.' and '-' are below '/'.
     */
    pthread_rwlock_rdlock(&ctx->lock);
    bsl_iter it = bsl_seek(ctx->index, prefix);
    while (it.key && !strncmp(it.key, prefix, prefix_len)) {
        const char *slash = strchr(it.key + prefix_len, '/');
        if (slash) {
            int n = (int)(slash - it.key);
            memcpy(after, it.key, n);
            after[n] = '/' + 1;
            after[n + 1] = '\0';
            it = bsl_seek(ctx->index, after);
            continue;
        }
        topk_add_live(&top, it.slot);
        bsl_next(&it);
    }
    fill_hits(&top, hits, nhits);
    pthread_rwlock_unlock(&ctx->lock);

    return top.total;
}

static int search_prefix(search_ctx *ctx, const char *prefix,
                    search_hit *hits, int *nhits)
{
    struct topk_results top = {0};

    pthread_rwlock_rdlock(&ctx->lock);
    bsl_prefix_for_each(ctx->index, prefix, topk_from_index_cb, &top);
    fill_hits(&top, hits, nhits);
    pthread_rwlock_unlock(&ctx->lock);

    return top.total;
}

static int search_token(search_ctx *ctx, const char *query,
                     search_hit *hits, int *nhits)
{
    struct topk_results top = {.query = query};
    char probe_buf[PATH_MAX];

    pthread_rwlock_rdlock(&ctx->lock);
    const char *probe = choose_probe(ctx->tokens, query, probe_buf, sizeof(probe_buf));
    if (probe)
        token_index_search(ctx->tokens, probe, topk_from_tokens_cb, &top);
    else
        bsl_for_each(ctx->index, topk_from_all_cb, &top);
    fill_hits(&top, hits, nhits);
    pthread_rwlock_unlock(&ctx->lock);

    return top.total;
}

int search_query(search_ctx *ctx, const char *text, search_hit *hits, int *nhits)
{
    size_t len = strlen(text);
    if (!len)
        return search_root(ctx, hits, nhits);

    if (len == 1 || strchr(text, '/')) {
        char prefix[PATH_MAX];
        if (text[0] == '/')
            snprintf(prefix, sizeof(prefix), "%s", text);
        else
            path_join(prefix, sizeof(prefix), ctx->root_path, text);   /* truncated if absurd */
        return search_prefix(ctx, prefix, hits, nhits);
    }
    return search_token(ctx, text, hits, nhits);
}
