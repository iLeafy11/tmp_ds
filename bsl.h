#ifndef BSL_H
#define BSL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BSL_KEYS_PER_LEAF 64
#define BSL_MAX_LEVEL 16
#define BSL_P (1.0 / 2.0)

/**
 * container_of() - Calculate address of object that contains address ptr
 * @ptr: pointer to member variable
 * @type: type of the structure containing ptr
 * @member: name of the member variable in struct @type
 *
 * Return: @type pointer of object containing ptr
 */
#ifndef container_of
#define container_of(ptr, type, member)                            \
    __extension__({                                                \
        const __typeof__(((type *) 0)->member) *__pmember = (ptr); \
        (type *) ((char *) __pmember - offsetof(type, member));    \
    })
#endif

/* Cache-friendly B-Skiplist (string key version).
 *
 * Each leaf holds up to BSL_KEYS_PER_LEAF sorted string pointers in a contiguous array, separate
 * from the data slots. Searching within a leaf scans the pointer array without touching the data.
 */

typedef struct bsl_node {
    int level;
    struct bsl_node *forward[];          /* forward[i] = next leaf at level i */
} bsl_node;

typedef struct bsl_leaf {
    int count;
    int right_inserts;
    int total_inserts;
    uint64_t tombstones;
    char *keys[BSL_KEYS_PER_LEAF];       /* sorted string pointers */
    void *slots[BSL_KEYS_PER_LEAF];      /* data pointers, parallel to keys */
    bsl_node node;
} bsl_leaf;

typedef struct bsl {
    bsl_node *head;
    int level;
    int size;
} bsl;

bsl *bsl_create(void);
void bsl_destroy(bsl *sl, void (*free_slot)(void *));

int bsl_insert(bsl *sl, const char *key, void *data);
int bsl_delete(bsl *sl, const char *key, void (*free_slot)(void *));
void *bsl_lookup(bsl *sl, const char *key);

/* Address of a live key's slot, so the caller can replace the value in place (a growable value that
 * moves on realloc). NULL if key is absent. Valid until the next modification of the skiplist.
 */
void **bsl_lookup_ref(bsl *sl, const char *key);

/* Iterate all live entries whose key starts with prefix. Callback receives (key, slot, user_arg).
 * Returns the total number of matching entries.
 */
int bsl_prefix_for_each(bsl *sl, const char *prefix,
                        void (*cb)(const char *key, void *slot, void *arg),
                        void *arg);

/* Iterate all live entries. Callback receives (key, slot, user_arg). */
void bsl_for_each(bsl *sl,
                  void (*cb)(const char *key, void *slot, void *arg),
                  void *arg);

/* Ordered cursor over live entries. key and slot mirror the current entry; key is NULL once the
 * cursor has run off the end. Only valid while the skiplist is not modified.
 */
typedef struct bsl_iter {
    bsl_node *node;
    int pos;
    const char *key;
    void *slot;
} bsl_iter;

/* Position a cursor on the first live entry whose key is >= key. */
bsl_iter bsl_seek(bsl *sl, const char *key);

/* Advance to the next live entry. */
void bsl_next(bsl_iter *it);

/* Insert key directly behind the cursor's entry and move the cursor onto it, skipping the top-down
 * search. The caller promises cursor key < key < next live key; anything else (including a
 * duplicate live key or an exhausted cursor) returns -1 and inserts nothing, so a caller feeding a
 * sorted run can fall back to bsl_insert when the promise does not hold. Amortised O(1): only the
 * leaf split or creation every BSL_KEYS_PER_LEAF inserts walks the list.
 */
int bsl_insert_after(bsl *sl, bsl_iter *it, const char *key, void *data);

/* Seed the PRNG used by random_level(). Call once at startup. */
void bsl_init_random(void);

#endif /* BSL_H */
