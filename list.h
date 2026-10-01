#ifndef _LIST_H
#define _LIST_H
#include <stddef.h>

/* Linux-kernel-style intrusive lists, trimmed to what this project uses. */

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

/* Circular doubly-linked list with a two-pointer head. */

struct list_head {
    struct list_head *prev, *next;
};

#define LIST_HEAD(head) struct list_head head = {&(head), &(head)}

#define INIT_LIST_HEAD(head) do { \
    (head)->next = (head); (head)->prev = (head); \
} while (0)

static inline void __list_add(struct list_head *new,
        struct list_head *prev, struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    prev->next = new;
}

static inline void list_add(struct list_head *node, struct list_head *head)
{
    __list_add(node, head, head->next);
}

static inline void list_add_tail(struct list_head *node, struct list_head *head)
{
    __list_add(node, head->prev, head);
}

static inline void list_remove(struct list_head *node)
{
    node->next->prev = node->prev;
    node->prev->next = node->next;
    node->next = NULL;
    node->prev = NULL;
}

static inline int list_empty(const struct list_head *head)
{
    return head->next == head;
}

#define list_entry(node, type, member) container_of(node, type, member)

#define list_for_each(node, head) \
    for (node = (head)->next; node != (head); node = node->next)

/* Safe against removal of the current node. */
#define list_for_each_safe(pos, n, head) \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

/* Doubly-linked list with a single-pointer head, for hash table buckets. A node's pprev points at
 * whatever pointer leads to it (the head's first or the previous node's next), so removal never
 * needs the head.
 */

struct hlist_head {
    struct hlist_node *first;
};

struct hlist_node {
    struct hlist_node *next, **pprev;
};

#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)

static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
    h->next = NULL;
    h->pprev = NULL;
}

static inline int hlist_empty(const struct hlist_head *h)
{
    return !h->first;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;
    n->next = first;
    if (first)
        first->pprev = &n->next;
    h->first = n;
    n->pprev = &h->first;
}

static inline void hlist_del(struct hlist_node *n)
{
    struct hlist_node *next = n->next;
    struct hlist_node **pprev = n->pprev;

    *pprev = next;
    if (next)
        next->pprev = pprev;
    n->next = NULL;
    n->pprev = NULL;
}

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_entry_safe(ptr, type, member)                  \
    __extension__({                                          \
        __typeof__(ptr) ____ptr = (ptr);                     \
        ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
    })

#define hlist_for_each_entry(pos, head, member)                                      \
    for (pos = hlist_entry_safe((head)->first, __typeof__(*(pos)), member); pos;     \
         pos = hlist_entry_safe((pos)->member.next, __typeof__(*(pos)), member))

/* Safe against removal of the current node. */
#define hlist_for_each_entry_safe(pos, n, head, member)                        \
    for (pos = hlist_entry_safe((head)->first, __typeof__(*pos), member);      \
         pos && ({ n = pos->member.next; 1; });                                \
         pos = hlist_entry_safe(n, __typeof__(*pos), member))

#endif
