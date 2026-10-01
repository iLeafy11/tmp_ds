#ifndef _HTABLE_H
#define _HTABLE_H

#include <stdint.h>
#include <stdlib.h>

#include "list.h"

/* Intrusive chained hash table.
 *
 * Users embed a ht_node in their own struct and supply a hash function and a key comparator. The
 * table computes the bucket from the hash, so n_buckets must be a power of two. When the number of
 * nodes exceeds the number of buckets the table doubles and rehashes; since nodes are intrusive
 * this only relinks them, nothing is copied.
 */
typedef uint32_t hash_t;

struct ht_node {
    hash_t hash;
    struct hlist_node list;
};

typedef hash_t hashfunc_t(const void *key);
typedef int cmp_t(const struct ht_node *n, const void *key);

struct htable {
    hashfunc_t *hashfunc;
    cmp_t *cmp;
    uint32_t n_buckets;
    uint32_t count;
    struct hlist_head *buckets;
};

static inline struct hlist_head *ht_alloc_buckets(uint32_t n)
{
    struct hlist_head *b = malloc(n * sizeof(*b));
    if (!b)
        return NULL;
    for (uint32_t i = 0; i < n; i++)
        INIT_HLIST_HEAD(&b[i]);
    return b;
}

/* n_buckets must be a power of two.
 *
 * Returns 0 on success, -1 on allocation failure; a failed table is empty (zero buckets) and still
 * safe to iterate and destroy.
 */
static inline int ht_init(struct htable *h, hashfunc_t *hashfunc, cmp_t *cmp,
                          uint32_t n_buckets)
{
    h->hashfunc = hashfunc;
    h->cmp = cmp;
    h->count = 0;
    h->buckets = ht_alloc_buckets(n_buckets);
    h->n_buckets = h->buckets ? n_buckets : 0;
    return h->buckets ? 0 : -1;
}

/* Frees the bucket array only. Nodes are owned by the caller. */
static inline void ht_destroy(struct htable *h)
{
    free(h->buckets);
    h->buckets = NULL;
    h->n_buckets = 0;
    h->count = 0;
}

static inline struct hlist_head *ht_bucket(const struct htable *h, hash_t hash)
{
    return &h->buckets[hash & (h->n_buckets - 1)];
}

static inline struct ht_node *ht_find(const struct htable *h, const void *key)
{
    hash_t hash = h->hashfunc(key);
    struct ht_node *n;

    hlist_for_each_entry(n, ht_bucket(h, hash), list) {
        if (n->hash == hash && !h->cmp(n, key))
            return n;
    }
    return NULL;
}

/* Doubles the bucket count and relinks every node. Failure leaves the table unchanged. */
static inline int ht_grow(struct htable *h)
{
    uint32_t new_n = h->n_buckets << 1;
    struct hlist_head *new_buckets = ht_alloc_buckets(new_n);
    if (!new_buckets)
        return -1;

    for (uint32_t i = 0; i < h->n_buckets; i++) {
        struct ht_node *n;
        struct hlist_node *tmp;
        hlist_for_each_entry_safe(n, tmp, &h->buckets[i], list)
            hlist_add_head(&n->list, &new_buckets[n->hash & (new_n - 1)]);
    }

    free(h->buckets);
    h->buckets = new_buckets;
    h->n_buckets = new_n;
    return 0;
}

/* Inserts n under key. Returns 0 on success, -1 if an equal key is already present. */
static inline int ht_insert(struct htable *h, struct ht_node *n, const void *key)
{
    if (ht_find(h, key))
        return -1;

    if (h->count >= h->n_buckets)
        ht_grow(h);  /* on failure the chains just get longer */

    n->hash = h->hashfunc(key);
    hlist_add_head(&n->list, ht_bucket(h, n->hash));
    h->count++;
    return 0;
}

static inline void ht_remove(struct htable *h, struct ht_node *n)
{
    hlist_del(&n->list);
    h->count--;
}

/**
 * ht_for_each_entry_safe - iterate over every node in the table, safe against removal
 * @pos: the type * to use as a loop cursor
 * @tmp: a &struct hlist_node to use as temporary storage
 * @i: a uint32_t to use as the bucket cursor
 * @h: the table
 * @member: the name of the ht_node within the struct
 */
#define ht_for_each_entry_safe(pos, tmp, i, h, member)                          \
    for (i = 0; i < (h)->n_buckets; i++)                                        \
        hlist_for_each_entry_safe(pos, tmp, &(h)->buckets[i], member.list)

#endif /* _HTABLE_H */
