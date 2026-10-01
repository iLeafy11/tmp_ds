#include "token_index.h"
#include "bsl.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* One bsl key per distinct suffix, whose value is the posting list of every entry containing it.
 * The suffix is stored once no matter how many files share it, the same effect as interning the
 * strings; the bsl (rather than a hash table) is what keeps the suffixes ordered for prefix search.
 *
 * A posting list is an unordered array that only grows: add appends, search deduplicates by entry
 * anyway, and deletion is the caller's dead flag on the entry. Per suffix the scan path is O(1).
 * Shortest token, in bytes: one ASCII letter is not a token (the UI routes a single byte to the
 * path prefix search anyway), while one CJK character (three bytes) is, so that a name such as
 * 年度報告.txt can be found by its last character.
 */
typedef struct posting {
    int n, cap;
    void *entries[];
} posting;

/* Appends entry, growing by doubling from a single slot since most suffixes belong to one file.
 * Returns the (possibly moved) list, or NULL on allocation failure with p untouched.
 */
static posting *posting_push(posting *p, void *entry)
{
    if (!p) {
        p = malloc(sizeof(*p) + sizeof(void *));
        if (!p)
            return NULL;
        p->n = 0;
        p->cap = 1;
    } else if (p->n == p->cap) {
        posting *np = realloc(p, sizeof(*p) + 2 * (size_t)p->cap * sizeof(void *));
        if (!np)
            return NULL;
        p = np;
        p->cap *= 2;
    }
    p->entries[p->n++] = entry;
    return p;
}

/* The index holds no mutable search state, so concurrent searches under a shared read lock are
 * safe.
 */
struct token_index {
    bsl *index;
    int n_tokens;      /* (suffix, entry) pairs over all posting lists */
};

token_index *token_index_create(void)
{
    token_index *ti = calloc(1, sizeof(*ti));
    if (!ti)
        return NULL;
    ti->index = bsl_create();
    if (!ti->index) {
        free(ti);
        return NULL;
    }
    return ti;
}

void token_index_destroy(token_index *ti)
{
    if (!ti)
        return;
    bsl_destroy(ti->index, free);
    free(ti);
}

/* Suffixes start only at code-point boundaries: the next one is past every continuation byte
 * (10xxxxxx). Names are well-formed UTF-8 by the time they get here (the scan skips the others), so
 * nothing needs decoding. No Unicode normalization either.
 */
static int next_code_point(const char *s, int j, int len)
{
    do
        j++;
    while (j < len && ((unsigned char)s[j] & 0xC0) == 0x80);
    return j;
}

/* Visits every searchable token of a basename, each position exactly once:
 *   - the complete basename, so an exact full-name query still works, including structural
 *     separators and the extension;
 *   - every suffix of every segment between structural boundaries, starting at code-point
 *     boundaries and at least TOKEN_MIN_BYTES long. Suffixes never cross a boundary.
 * A basename without any boundary is its own single segment, and that segment's first suffix is the
 * complete basename again, so it is skipped.
 */
static void for_each_token(const char *base,
                           void (*cb)(const char *token, int len, void *arg), void *arg)
{
    int blen = (int)strlen(base);

    if (blen >= TOKEN_MIN_BYTES)
        cb(base, blen, arg);

    int start = 0;
    for (int i = 0; i <= blen; i++) {
        if (i < blen && !token_is_boundary(base[i]))
            continue;

        const char *seg = base + start;
        int seg_len = i - start;
        bool seg_is_whole_name = start == 0 && i == blen;

        for (int j = 0; seg_len - j >= TOKEN_MIN_BYTES; ) {
            if (!(seg_is_whole_name && j == 0))
                cb(seg + j, seg_len - j, arg);
            j = next_code_point(seg, j, seg_len);
        }
        start = i + 1;
    }
}

/* Per-call set of the tokens already handled for one basename, so a suffix that the basename
 * produces twice ("ab.ab") touches its posting list once. Tokens are substrings of the basename, so
 * a slot is just an offset and length. Fixed size on the stack: a basename is at most NAME_MAX
 * bytes, hence at most NAME_MAX tokens, and TOKEN_SET_SLOTS keeps the load factor of the linear
 * probing under one half. Should a longer name turn up anyway, the set stops deduplicating once
 * full; a repeated entry in a list only costs a slot, search deduplicates.
 */
#define TOKEN_SET_SLOTS 512
#define TOKEN_SET_MAX (TOKEN_SET_SLOTS / 2)

struct token_set {
    const char *base;
    int n;
    struct { uint16_t off, len; } slots[TOKEN_SET_SLOTS];   /* len 0 = empty */
};

static void token_set_init(struct token_set *ts, const char *base, int blen)
{
    ts->base = base;
    ts->n = blen > UINT16_MAX ? TOKEN_SET_MAX : 0;
    memset(ts->slots, 0, sizeof(ts->slots));
}

/* Returns true if token was not seen before in this call. Hashing every suffix of the name costs
 * O(sum of suffix lengths), bounded by NAME_MAX; a prefix-hash table would make it O(name length)
 * if that ever shows up.
 */
static bool token_set_insert(struct token_set *ts, const char *token, int len)
{
    if (ts->n >= TOKEN_SET_MAX)
        return true;

    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++)
        h = (h ^ (unsigned char)token[i]) * 16777619u;

    int off = (int)(token - ts->base);
    for (int idx = h & (TOKEN_SET_SLOTS - 1); ; idx = (idx + 1) & (TOKEN_SET_SLOTS - 1)) {
        if (!ts->slots[idx].len) {
            ts->slots[idx].off = (uint16_t)off;
            ts->slots[idx].len = (uint16_t)len;
            ts->n++;
            return true;
        }
        if (ts->slots[idx].len == len && !memcmp(ts->base + ts->slots[idx].off, token, len))
            return false;
    }
}

struct add_state {
    token_index *ti;
    void *entry;
    struct token_set seen;
};

/* Tokens are not NUL-terminated (a segment suffix ends at a boundary character), so the bsl key is
 * a copy.
 *
 * Returns false for a token too long to be a key.
 */
static bool token_key(char *key, const char *token, int len)
{
    if (len > NAME_MAX)
        return false;
    memcpy(key, token, len);
    key[len] = '\0';
    return true;
}

static void add_token_cb(const char *token, int len, void *arg)
{
    struct add_state *s = arg;
    char key[NAME_MAX + 1];
    if (!token_set_insert(&s->seen, token, len) || !token_key(key, token, len))
        return;

    void **ref = bsl_lookup_ref(s->ti->index, key);
    posting *p = posting_push(ref ? *ref : NULL, s->entry);
    if (!p)
        return;
    if (ref) {
        *ref = p;
    } else if (bsl_insert(s->ti->index, key, p)) {
        free(p);
        return;
    }
    s->ti->n_tokens++;
}

void token_index_add(token_index *ti, const char *path, void *entry)
{
    struct add_state state = {.ti = ti, .entry = entry};
    const char *base = token_basename(path);
    token_set_init(&state.seen, base, (int)strlen(base));
    for_each_token(base, add_token_cb, &state);
}

int token_index_count(token_index *ti)
{
    return ti->n_tokens;
}

/* Per-search open-addressing set of entry pointers, used to report each entry once even when
 * several of its suffixes match the query. (token_set above is the other set in this file: per add,
 * of tokens; this one is per search, of entries.)
 */
struct entry_set {
    void **slots;
    int cap;
    int shift;      /* 64 - log2(cap): the product's top log2(cap) bits pick the slot */
    int n;
};

#define ENTRY_SET_INITIAL_CAP 128
#define ENTRY_SET_INITIAL_SHIFT (64 - 7)

static void entry_set_init(struct entry_set *es)
{
    es->cap = ENTRY_SET_INITIAL_CAP;
    es->shift = ENTRY_SET_INITIAL_SHIFT;
    es->n = 0;
    es->slots = calloc(ENTRY_SET_INITIAL_CAP, sizeof(void *));
}

static void entry_set_free(struct entry_set *es)
{
    free(es->slots);
}

/* Index of ptr's slot, or of the empty slot it would take. Fibonacci hashing: multiply by 2^64 /
 * phi and keep the top bits.
 *
 * The pointer is divided by malloc's alignment first. Its low bits are always zero, and a
 * product's low bits depend only on the factors' low bits: masking the low bits of the product (as
 * this once did) put every entry on a multiple of 16, about eight extra probes per insert. Keeping
 * the top bits without the division is not enough either: the multiplier becomes 16 / phi, which
 * loses the even spread of phi and resonates with some table sizes (eight extra probes at 20000
 * entries, one at 2000). Divided, 0.6 to 0.9 at every size from 2000 to a million, below a random
 * hash: heap addresses are close to an arithmetic progression, which phi spreads most evenly.
 */
static int entry_set_probe(void **slots, int cap, int shift, void *ptr)
{
    uint64_t v = (uintptr_t)ptr / _Alignof(max_align_t);
    int idx = (int)((v * 11400714819323198485ull) >> shift);
    while (slots[idx] && slots[idx] != ptr)
        idx = (idx + 1) & (cap - 1);
    return idx;
}

/* Returns true if ptr was not seen before. With no table (allocation failed) every pointer counts
 * as new, so the search degrades to possibly repeated results rather than failing; likewise once a
 * table that could not grow is about to fill, since probing needs an empty slot to stop at.
 */
static bool entry_set_insert(struct entry_set *es, void *ptr)
{
    if (!es->slots || es->n + 1 >= es->cap)
        return true;

    if (es->n * 10 > es->cap * 7) {
        int new_cap = es->cap << 1;
        void **ns = calloc(new_cap, sizeof(void *));
        if (ns) {
            for (int i = 0; i < es->cap; i++) {
                if (es->slots[i])
                    ns[entry_set_probe(ns, new_cap, es->shift - 1, es->slots[i])] = es->slots[i];
            }
            free(es->slots);
            es->slots = ns;
            es->cap = new_cap;
            es->shift--;
        }
    }

    int idx = entry_set_probe(es->slots, es->cap, es->shift, ptr);
    if (es->slots[idx])
        return false;
    es->slots[idx] = ptr;
    es->n++;
    return true;
}

struct search_state {
    void (*cb)(void *entry, void *arg);
    void *arg;
    struct entry_set *seen;
    int unique;
};

static void search_posting_cb(const char *key, void *slot, void *arg)
{
    (void)key;
    struct search_state *s = arg;
    posting *p = slot;

    for (int i = 0; i < p->n; i++) {
        if (!entry_set_insert(s->seen, p->entries[i]))
            continue;
        s->unique++;
        s->cb(p->entries[i], s->arg);
    }
}

int token_index_prefix_postings(token_index *ti, const char *prefix, int limit)
{
    size_t plen = strlen(prefix);
    int count = 0;

    for (bsl_iter it = bsl_seek(ti->index, prefix); it.key && !strncmp(it.key, prefix, plen);
         bsl_next(&it)) {
        count += ((posting *)it.slot)->n;
        if (limit >= 0 && count > limit)
            break;
    }
    return count;
}

int token_index_search(token_index *ti, const char *query,
                       void (*cb)(void *entry, void *arg), void *arg)
{
    struct entry_set seen;
    entry_set_init(&seen);

    struct search_state state = {
        .cb = cb,
        .arg = arg,
        .seen = &seen,
        .unique = 0,
    };

    bsl_prefix_for_each(ti->index, query, search_posting_cb, &state);

    entry_set_free(&seen);
    return state.unique;
}
