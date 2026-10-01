#include "bsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Node/leaf helpers */
#define leaf_entry(ptr) container_of(ptr, bsl_leaf, node)

static char *xstrdup(const char *s)
{
    char *dup = strdup(s);
    if (!dup) {
        perror("strdup");
        abort();
    }
    return dup;
}

static bsl_node *leaf_alloc(int level)
{
    bsl_leaf *leaf = calloc(1, sizeof(bsl_leaf) + level * sizeof(bsl_node *));
    if (!leaf) return NULL;
    leaf->node.level = level;
    return &leaf->node;
}

/* Head sentinel: only bsl_node, no keys/slots */
static bsl_node *head_alloc(int level)
{
    bsl_node *head = calloc(1, sizeof(bsl_node) + level * sizeof(bsl_node *));
    if (head) head->level = level;
    return head;
}

static void leaf_free(bsl_node *n, void (*free_slot)(void *))
{
    if (!n) return;
    bsl_leaf *leaf = leaf_entry(n);
    for (int i = 0; i < leaf->count; i++) {
        free(leaf->keys[i]);
        if (free_slot && leaf->slots[i])
            free_slot(leaf->slots[i]);
    }
    free(leaf);
}

/* Random level */
static int random_level(void)
{
    long threshold = (long)(BSL_P * 0x7fffffff);
    int lv = 1;
    while (random() < threshold && lv < BSL_MAX_LEVEL)
        lv++;
    return lv;
}

/* Binary search within a leaf */
static int leaf_search(bsl_leaf *leaf, const char *key, bool *found)
{
    int lo = 0, hi = leaf->count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int cmp = strcmp(leaf->keys[mid], key);
        if (!cmp) {
            *found = true;
            return mid;
        }
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    *found = false;
    return lo;
}

/* Insert key+slot at position pos within a leaf */
static void leaf_insert_at(bsl_leaf *leaf, int pos, char *key, void *data)
{
    leaf->total_inserts++;
    if (pos >= leaf->count / 2)
        leaf->right_inserts++;
    int n = leaf->count - pos;
    if (n > 0) {
        memmove(&leaf->keys[pos + 1], &leaf->keys[pos], n * sizeof(char *));
        memmove(&leaf->slots[pos + 1], &leaf->slots[pos], n * sizeof(void *));
        uint64_t above = leaf->tombstones & ~((1ULL << pos) - 1);
        uint64_t below = leaf->tombstones & ((1ULL << pos) - 1);
        leaf->tombstones = below | (above << 1);
    }
    leaf->keys[pos] = key;
    leaf->slots[pos] = data;
    leaf->count++;
}

/* Compact: remove tombstoned entries, compress the arrays */
static void leaf_compact(bsl_leaf *leaf)
{
    int dst = 0;
    for (int src = 0; src < leaf->count; src++) {
        if (leaf->tombstones & (1ULL << src)) {
            free(leaf->keys[src]);
            continue;
        }
        if (dst != src) {
            leaf->keys[dst] = leaf->keys[src];
            leaf->slots[dst] = leaf->slots[src];
        }
        dst++;
    }
    for (int i = dst; i < leaf->count; i++) {
        leaf->keys[i] = NULL;
        leaf->slots[i] = NULL;
    }
    leaf->count = dst;
    leaf->tombstones = 0;
    leaf->right_inserts = leaf->total_inserts = 0;
}

/* Skiplist core */
bsl *bsl_create(void)
{
    bsl *sl = calloc(1, sizeof(bsl));
    if (!sl) return NULL;

    sl->head = head_alloc(BSL_MAX_LEVEL);
    if (!sl->head) {
        free(sl);
        return NULL;
    }
    sl->level = 1;
    return sl;
}

void bsl_destroy(bsl *sl, void (*free_slot)(void *))
{
    if (!sl) return;

    bsl_node *cur = sl->head->forward[0];
    while (cur) {
        bsl_node *next = cur->forward[0];
        leaf_free(cur, free_slot);
        cur = next;
    }

    free(sl->head);  /* head is just a bsl_node, not embedded in a leaf */
    free(sl);
}

/* Find predecessor at level 0: last node whose max key < key */
static bsl_node *find_pred(bsl *sl, const char *key)
{
    bsl_node *cur = sl->head;

    for (int i = sl->level - 1; i >= 0; i--) {
        bsl_node *nxt;
        while ((nxt = cur->forward[i]) != NULL) {
            bsl_leaf *nxt_leaf = leaf_entry(nxt);
            if (!nxt_leaf->count ||
                strcmp(nxt_leaf->keys[nxt_leaf->count - 1], key) >= 0)
                break;
            cur = nxt;
        }
    }

    return cur;
}

/* Link a node into the skiplist at all its levels (top-down) */
static void link_node(bsl *sl, bsl_node *node)
{
    int lv = node->level;
    bsl_leaf *leaf = leaf_entry(node);
    const char *key = leaf->keys[0];

    if (lv > sl->level) sl->level = lv;

    bsl_node *cur = sl->head;
    for (int i = sl->level - 1; i >= 0; i--) {
        bsl_node *nxt;
        while ((nxt = cur->forward[i]) != NULL) {
            bsl_leaf *nxt_leaf = leaf_entry(nxt);
            if (!nxt_leaf->count ||
                strcmp(nxt_leaf->keys[nxt_leaf->count - 1], key) >= 0)
                break;
            cur = nxt;
        }
        if (i < lv) {
            node->forward[i] = cur->forward[i];
            cur->forward[i] = node;
        }
    }
}

/* Unlink a node from all levels */
static void unlink_node(bsl *sl, bsl_node *node, const char *key)
{
    bsl_node *cur = sl->head;
    for (int i = sl->level - 1; i >= 0; i--) {
        while (cur->forward[i] && cur->forward[i] != node) {
            bsl_leaf *nxt_leaf = leaf_entry(cur->forward[i]);
            if (!nxt_leaf->count ||
                strcmp(nxt_leaf->keys[nxt_leaf->count - 1], key) >= 0)
                break;
            cur = cur->forward[i];
        }
        if (cur->forward[i] == node)
            cur->forward[i] = node->forward[i];
    }
    while (sl->level > 1 && !sl->head->forward[sl->level - 1])
        sl->level--;
}

void **bsl_lookup_ref(bsl *sl, const char *key)
{
    bsl_node *n = find_pred(sl, key)->forward[0];
    if (!n) return NULL;

    bsl_leaf *leaf = leaf_entry(n);
    if (!leaf->count) return NULL;

    bool found;
    int pos = leaf_search(leaf, key, &found);
    if (!found || (leaf->tombstones & (1ULL << pos)))
        return NULL;
    return &leaf->slots[pos];
}

void *bsl_lookup(bsl *sl, const char *key)
{
    void **ref = bsl_lookup_ref(sl, key);
    return ref ? *ref : NULL;
}

/* Split leaf data at randomized pivot; no pointer changes */
static bsl_node *leaf_split(bsl_node *old_node, int new_level)
{
    bsl_node *new_node = leaf_alloc(new_level);
    if (!new_node) return NULL;

    bsl_leaf *old_leaf = leaf_entry(old_node);
    bsl_leaf *new_leaf = leaf_entry(new_node);

    bool sequential = old_leaf->total_inserts > 0 &&
                      old_leaf->right_inserts * 100 / old_leaf->total_inserts >= 80;
    int mid;
    if (sequential) {
        int lo = old_leaf->count * 4 / 5;
        int hi = old_leaf->count * 19 / 20;
        mid = lo + (random() % (hi - lo + 1));
    } else {
        mid = old_leaf->count / 2;
    }
    int move = old_leaf->count - mid;

    memcpy(new_leaf->keys, &old_leaf->keys[mid], move * sizeof(char *));
    memcpy(new_leaf->slots, &old_leaf->slots[mid], move * sizeof(void *));
    new_leaf->count = move;

    memset(&old_leaf->keys[mid], 0, move * sizeof(char *));
    memset(&old_leaf->slots[mid], 0, move * sizeof(void *));
    old_leaf->count = mid;

    old_leaf->right_inserts = old_leaf->total_inserts = 0;
    new_leaf->right_inserts = new_leaf->total_inserts = 0;

    return new_node;
}

/* Bring a tombstoned key back to life with a new slot. */
static void leaf_resurrect(bsl *sl, bsl_leaf *leaf, int pos, void *data)
{
    leaf->tombstones &= ~(1ULL << pos);
    leaf->slots[pos] = data;
    sl->size++;
}

/* A fresh leaf holding only key, linked into the list top-down. */
static bsl_node *leaf_new_single(bsl *sl, const char *key, void *data)
{
    bsl_node *node = leaf_alloc(random_level());
    if (!node)
        return NULL;
    bsl_leaf *leaf = leaf_entry(node);
    leaf->keys[0] = xstrdup(key);
    leaf->slots[0] = data;
    leaf->count = 1;
    link_node(sl, node);
    sl->size++;
    return node;
}

/* node's leaf is full and does not hold key, whose insertion index there is *pos. Makes room by
 * compacting tombstones or splitting, and returns the node key now belongs in with *pos updated to
 * its index there. NULL on allocation failure.
 */
static bsl_node *leaf_make_room(bsl *sl, bsl_node *node, const char *key, int *pos)
{
    bsl_leaf *leaf = leaf_entry(node);

    if (leaf->tombstones) {
        bool found;
        leaf_compact(leaf);
        *pos = leaf_search(leaf, key, &found);
        return node;
    }

    bsl_node *new_node = leaf_split(node, random_level());
    if (!new_node)
        return NULL;
    link_node(sl, new_node);
    if (*pos > leaf->count) {    /* leaf->count is the split point now */
        *pos -= leaf->count;
        return new_node;
    }
    return node;
}

int bsl_insert(bsl *sl, const char *key, void *data)
{
    bsl_node *pred = find_pred(sl, key);
    bsl_node *next = pred->forward[0];

    /* Choose target: append to predecessor when key falls between nodes */
    bsl_node *target = next;
    if (pred != sl->head) {
        bsl_leaf *pl = leaf_entry(pred);
        if (pl->count > 0 && pl->count < BSL_KEYS_PER_LEAF &&
            strcmp(key, pl->keys[pl->count - 1]) > 0 &&
            (!next || !leaf_entry(next)->count ||
             strcmp(key, leaf_entry(next)->keys[0]) < 0))
            target = pred;
    }

    if (!target || !leaf_entry(target)->count)
        return leaf_new_single(sl, key, data) ? 0 : -1;

    bsl_leaf *leaf = leaf_entry(target);
    bool found;
    int pos = leaf_search(leaf, key, &found);
    if (found) {
        if (!(leaf->tombstones & (1ULL << pos)))
            return -1;
        leaf_resurrect(sl, leaf, pos, data);
        return 0;
    }

    if (leaf->count >= BSL_KEYS_PER_LEAF) {
        target = leaf_make_room(sl, target, key, &pos);
        if (!target)
            return -1;
        leaf = leaf_entry(target);
    }
    leaf_insert_at(leaf, pos, xstrdup(key), data);
    sl->size++;
    return 0;
}

int bsl_delete(bsl *sl, const char *key, void (*free_slot)(void *))
{
    bsl_node *node = find_pred(sl, key)->forward[0];
    if (!node) return -1;

    bsl_leaf *leaf = leaf_entry(node);
    if (!leaf->count) return -1;

    bool found;
    int pos = leaf_search(leaf, key, &found);
    if (!found || (leaf->tombstones & (1ULL << pos)))
        return -1;

    const char *internal_key = leaf->keys[pos];
    if (free_slot) free_slot(leaf->slots[pos]);
    leaf->slots[pos] = NULL;
    leaf->tombstones |= 1ULL << pos;
    sl->size--;

    int alive = leaf->count - __builtin_popcountll(leaf->tombstones);
    if (!alive) {
        unlink_node(sl, node, internal_key);
        for (int i = 0; i < leaf->count; i++)
            free(leaf->keys[i]);
        free(leaf);
    }

    return 0;
}

int bsl_prefix_for_each(bsl *sl, const char *prefix,
                        void (*cb)(const char *key, void *slot, void *arg),
                        void *arg)
{
    bsl_node *node = find_pred(sl, prefix)->forward[0];
    int prefix_len = strlen(prefix);
    int count = 0;

    while (node) {
        bsl_leaf *leaf = leaf_entry(node);
        for (int i = 0; i < leaf->count; i++) {
            int cmp = strncmp(leaf->keys[i], prefix, prefix_len);
            if (cmp > 0)
                return count;
            if (!cmp && !(leaf->tombstones & (1ULL << i))) {
                if (cb)
                    cb(leaf->keys[i], leaf->slots[i], arg);
                count++;
            }
        }
        node = node->forward[0];
    }

    return count;
}

void bsl_for_each(bsl *sl,
                  void (*cb)(const char *key, void *slot, void *arg),
                  void *arg)
{
    bsl_node *cur = sl->head->forward[0];
    while (cur) {
        bsl_leaf *leaf = leaf_entry(cur);
        for (int i = 0; i < leaf->count; i++) {
            if (!(leaf->tombstones & (1ULL << i)))
                cb(leaf->keys[i], leaf->slots[i], arg);
        }
        cur = cur->forward[0];
    }
}

/* Move the cursor forward from (node, pos) to the first live slot at or after it, crossing leaves
 * as needed, and load key/slot. Exhausted cursors get key = NULL.
 */
static void iter_settle(bsl_iter *it)
{
    while (it->node) {
        bsl_leaf *leaf = leaf_entry(it->node);
        for (; it->pos < leaf->count; it->pos++) {
            if (!(leaf->tombstones & (1ULL << it->pos))) {
                it->key = leaf->keys[it->pos];
                it->slot = leaf->slots[it->pos];
                return;
            }
        }
        it->node = it->node->forward[0];
        it->pos = 0;
    }
    it->key = NULL;
    it->slot = NULL;
}

bsl_iter bsl_seek(bsl *sl, const char *key)
{
    bsl_iter it = {.node = find_pred(sl, key)->forward[0], .pos = 0};
    if (it.node) {
        bool found;
        it.pos = leaf_search(leaf_entry(it.node), key, &found);
    }
    iter_settle(&it);
    return it;
}

void bsl_next(bsl_iter *it)
{
    if (!it->node)
        return;
    it->pos++;
    iter_settle(it);
}

int bsl_insert_after(bsl *sl, bsl_iter *it, const char *key, void *data)
{
    if (!it->node || !it->key || strcmp(it->key, key) >= 0)
        return -1;
    bsl_iter peek = *it;
    bsl_next(&peek);
    if (peek.key && strcmp(key, peek.key) >= 0)
        return -1;

    bsl_node *node = it->node;
    bsl_leaf *leaf = leaf_entry(node);
    bool found = false;
    int pos;

    /* Tombstoned keys still take part in the leaf's ordering, so with any present the slot has to
     * be searched for; otherwise it is simply the one behind the cursor.
     */
    if (leaf->tombstones)
        pos = leaf_search(leaf, key, &found);
    else
        pos = it->pos + 1;

    if (found) {
        /* Live keys were excluded above, so this is a tombstone. */
        leaf_resurrect(sl, leaf, pos, data);
        goto done;
    }

    if (leaf->count >= BSL_KEYS_PER_LEAF) {
        if (!leaf->tombstones && pos == leaf->count) {
            /* Appending behind a full leaf: start a fresh one, as bsl_insert does. */
            node = leaf_new_single(sl, key, data);
            if (!node)
                return -1;
            leaf = leaf_entry(node);
            pos = 0;
            goto done;
        }
        node = leaf_make_room(sl, node, key, &pos);
        if (!node)
            return -1;
        leaf = leaf_entry(node);
    }

    leaf_insert_at(leaf, pos, xstrdup(key), data);
    sl->size++;

done:
    it->node = node;
    it->pos = pos;
    it->key = leaf->keys[pos];
    it->slot = data;
    return 0;
}

void bsl_init_random(void)
{
    srandom((unsigned)time(NULL));
}
