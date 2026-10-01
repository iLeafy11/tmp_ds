#ifndef PAGERANK_H
#define PAGERANK_H

#include <stdatomic.h>
#include <stdbool.h>

#include "threadpool.h"

#define PR_BLOCK_BITS 10
#define PR_BLOCK_SIZE (1 << PR_BLOCK_BITS)

/* Damping: the chance that after opening a file the walker follows a recorded transition rather
 * than starting afresh. 0.85 is the web's value, where a chain of links is a real path; a chain of
 * "opened next" is a weaker thing, and at 0.85 the last file of any chain outscores its first by
 * 2.6x for no reason. 0.5 halves that. Measured on synthetic chains, hubs and daily pairs; see
 * Dev_notes.
 */
#define PR_ALPHA 0.5

/* Residual threshold. Mass is in units of opens (one open injects 1), so 1e-4 of an open is far
 * below anything a ranking can tell apart; the old 1e-8 was scaled to residuals of 1/n. Too small
 * an epsilon makes a cycle of files spin: each step around it only loses (1 - alpha) of the mass.
 */
#define PR_EPSILON 1e-4

/* Personalized PageRank with an unnormalized teleport vector. Each node carries v = its decayed
 * count of opens; a full computation seeds r with v, nothing else. Because the solution is linear
 * in v, every change the daemon makes is cheap and exact:
 *   open a file        v_i += 1  ==  r_i += 1, then push from i
 *   time decay         v *= c    ==  p *= c and r *= c on every node, no push
 *   new node           adds a zero to v, changes nothing for anyone else
 *   edge weight change the existing incremental formula, which relies on the pushed-out total
 *                      being alpha/(1-alpha) * p_u, a relation scaling preserves
 * There is no 1/n anywhere, so no full recomputation is ever needed for growth or decay; one is
 * still done after a load (r is not persisted) and serves as the reference in the tests. A file
 * never opened has p = 0; files that are opened often gain score directly, not only as the
 * destination of transitions (a hub opened twenty times used to score like a file never opened,
 * while the tail of a one-off chain scored 3x).
 *
 * Displayed score: p / (1 - alpha), so a file opened once with no edges shows 1.0, in units of
 * opens.
 */
#define PR_SCORE(p) ((p) / (1.0 - PR_ALPHA))

/* Edges whose weight has decayed below this are not written to a snapshot, so they are gone after
 * the next restart; while running the graph never removes an edge.
 */
#define PR_EDGE_MIN_WEIGHT 0.01

/* Smallest power of two >= v (1 for v <= 1). Shared by the growable tables that are indexed by node
 * id and by the worklist ring, whose capacity must be a power of two.
 */
static inline int round_up_pow2(int v)
{
    return v <= 1 ? 1 : 1 << (32 - __builtin_clz((unsigned)(v - 1)));
}

typedef struct pr_node pr_node;

typedef struct pr_edge {
    pr_node *to;
    double weight;
} pr_edge;

struct pr_node {
    int id;
    _Atomic double r;
    _Atomic double p;
    double opens;           /* v: decayed count of opens; written under graph_lock for writing */
    pr_edge *out;
    int out_degree;
    int out_cap;
    atomic_bool in_worklist;
};

/* Pending edge-weight changes out of one node: each delta adds amount to the edge from -> to. */
typedef struct pr_edge_delta {
    pr_node *to;
    double amount;
} pr_edge_delta;

typedef struct pr_node_update {
    pr_node *from;
    pr_edge_delta *deltas;
    int n;
    int cap;
    double opens;           /* times `from` itself was opened in this batch */
} pr_node_update;

typedef struct pr_graph {
    pr_node **blocks;
    int n;
    int n_blocks;
} pr_graph;

static inline pr_node *pr_get(pr_graph *g, int id)
{
    return &g->blocks[id >> PR_BLOCK_BITS][id & (PR_BLOCK_SIZE - 1)];
}

pr_graph *pr_create(int initial_cap);
void pr_destroy(pr_graph *g);

int pr_add_node(pr_graph *g);
int pr_add_edge(pr_node *from, pr_node *to, double weight);

/* Time decay: multiplies every edge weight, every opens count, and every p and r by factor (0 <
 * factor < 1), so that a transition or an open recorded long ago counts for less than one recorded
 * today. Scaling the out-edges alike leaves transition probabilities as they were, and scaling v
 * scales the solution, so p and r are scaled along and the state stays exact. Errors accumulated in
 * p decay with it.
 */
void pr_decay(pr_graph *g, double factor);

/* Forgets every access: opens, scores and residuals to zero, every out-edge list emptied (the
 * arrays stay for reuse). The nodes stay, so the index keeps its ids.
 */
void pr_clear(pr_graph *g);

int pr_inc_edge(pr_node *from, pr_node *to, double amount, double *old_w);

/* Adds every delta of every update to the edge weights. Must precede pr_compute_incremental on the
 * same updates, which reads the new weights back from the graph. Applies a batch: edge increments,
 * and each update's opens onto its node.
 */
void pr_apply_updates(pr_node_update *updates, int n_updates);

void pr_compute_serial(pr_graph *g, double alpha, double epsilon);

/* The parallel variants run on a caller-owned pool, which must be idle when they are called and is
 * idle again when they return. A NULL pool falls back to the serial computation.
 */
void pr_compute_parallel(pr_graph *g, double alpha, double epsilon,
                         threadpool *tp);
void pr_compute_incremental(pr_graph *g, pr_node_update *updates,
                            int n_updates, double alpha, double epsilon,
                            threadpool *tp);

#endif /* PAGERANK_H */
