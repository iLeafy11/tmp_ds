#include "pagerank.h"
#include "threadpool.h"

#include <math.h>
#include <stdlib.h>

pr_graph *pr_create(int initial_cap)
{
    pr_graph *g = calloc(1, sizeof(pr_graph));
    if (!g) return NULL;

    int nb = (initial_cap + PR_BLOCK_SIZE - 1) / PR_BLOCK_SIZE;
    if (nb < 1) nb = 1;

    g->blocks = malloc(nb * sizeof(pr_node *));
    if (!g->blocks) {
        free(g);
        return NULL;
    }

    for (int i = 0; i < nb; i++) {
        g->blocks[i] = calloc(PR_BLOCK_SIZE, sizeof(pr_node));
        if (!g->blocks[i]) {
            for (int j = 0; j < i; j++) free(g->blocks[j]);
            free(g->blocks);
            free(g);
            return NULL;
        }
    }
    g->n_blocks = nb;
    return g;
}

void pr_destroy(pr_graph *g)
{
    if (!g) return;
    for (int i = 0; i < g->n; i++)
        free(pr_get(g, i)->out);
    for (int i = 0; i < g->n_blocks; i++)
        free(g->blocks[i]);
    free(g->blocks);
    free(g);
}

int pr_add_node(pr_graph *g)
{
    int block_idx = g->n >> PR_BLOCK_BITS;
    if (block_idx >= g->n_blocks) {
        pr_node **tmp = realloc(g->blocks,
                                (g->n_blocks + 1) * sizeof(pr_node *));
        if (!tmp) return -1;
        g->blocks = tmp;
        g->blocks[g->n_blocks] = calloc(PR_BLOCK_SIZE, sizeof(pr_node));
        if (!g->blocks[g->n_blocks]) return -1;
        g->n_blocks++;
    }
    pr_node *node = pr_get(g, g->n);
    node->id = g->n;
    return g->n++;
}

int pr_add_edge(pr_node *from, pr_node *to, double weight)
{
    if (from->out_degree >= from->out_cap) {
        int new_cap = from->out_cap ? from->out_cap << 1 : 4;
        pr_edge *tmp = realloc(from->out, new_cap * sizeof(pr_edge));
        if (!tmp) return -1;
        from->out = tmp;
        from->out_cap = new_cap;
    }
    from->out[from->out_degree++] = (pr_edge){.to = to, .weight = weight};
    return 0;
}

void pr_decay(pr_graph *g, double factor)
{
    for (int i = 0; i < g->n; i++) {
        pr_node *node = pr_get(g, i);
        for (int j = 0; j < node->out_degree; j++)
            node->out[j].weight *= factor;
        node->opens *= factor;
        atomic_store(&node->p, atomic_load(&node->p) * factor);
        atomic_store(&node->r, atomic_load(&node->r) * factor);
    }
}

void pr_clear(pr_graph *g)
{
    for (int i = 0; i < g->n; i++) {
        pr_node *node = pr_get(g, i);
        node->out_degree = 0;
        node->opens = 0.0;
        atomic_store(&node->p, 0.0);
        atomic_store(&node->r, 0.0);
    }
}

int pr_inc_edge(pr_node *from, pr_node *to, double amount, double *old_w)
{
    for (int i = 0; i < from->out_degree; i++) {
        if (from->out[i].to == to) {
            if (old_w) *old_w = from->out[i].weight;
            from->out[i].weight += amount;
            return 0;
        }
    }
    if (old_w) *old_w = 0.0;
    return pr_add_edge(from, to, amount);
}

void pr_apply_updates(pr_node_update *updates, int n_updates)
{
    for (int i = 0; i < n_updates; i++) {
        pr_node_update *u = &updates[i];
        for (int j = 0; j < u->n; j++)
            pr_inc_edge(u->from, u->deltas[j].to, u->deltas[j].amount, NULL);
        u->from->opens += u->opens;
    }
}

static double total_out_weight(pr_node *node)
{
    double sum = 0.0;
    for (int i = 0; i < node->out_degree; i++)
        sum += node->out[i].weight;
    return sum;
}

static inline void atomic_add_double(_Atomic double *target, double val)
{
    double old = atomic_load(target);
    while (!atomic_compare_exchange_weak(target, &old, old + val))
        ;
}

/* Push-based PageRank
 *
 * Every node carries settled mass p and residual r. Processing a node moves (1 - alpha) * r into p
 * and pushes alpha * r to its out-neighbours' residuals, weighted by edge weight. Nodes whose
 * residual is below epsilon are simply left alone: the residual stays in r, keeps accumulating, and
 * is processed once it crosses the threshold. It must never be folded into p directly, since that
 * would credit a node with mass that was neither scaled by (1 - alpha) nor propagated, and the
 * incremental update below relies on p being exactly the mass that has been pushed out.
 *
 * Active nodes wait in a FIFO ring. FIFO matters: a node that just received a little residual goes
 * to the back, so by the time it is processed more has accumulated and it is pushed once instead of
 * many times. (A stack does the opposite and is dramatically slower on dense graphs.)
 *
 * The serial computation uses one ring that grows as needed. The parallel one hands each task a
 * batch of nodes and a fixed-size ring that is private to the task, so it needs no locking; when it
 * fills up, the newest PR_BATCH nodes are handed to the pool as a new task. Sparse graphs, where
 * most nodes have no out-edges and processing them is trivial, thus cost almost nothing in
 * scheduling, while dense graphs keep spilling work to other workers.
 */
#define PR_BATCH PR_BLOCK_SIZE

/* cap is always a power of two, so an index wraps with a mask instead of a division. */
typedef struct ring {
    pr_node **buf;
    int cap;
    int head;
    int n;
} ring;

static bool ring_init(ring *q, int min_cap)
{
    q->cap = round_up_pow2(min_cap);
    q->buf = malloc((size_t)q->cap * sizeof(pr_node *));
    q->head = 0;
    q->n = 0;
    return q->buf != NULL;
}

/* Slot of the i-th element counted from the head. */
static pr_node **ring_at(ring *q, int i)
{
    return &q->buf[(q->head + i) & (q->cap - 1)];
}

static void ring_free(ring *q)
{
    free(q->buf);
}

static bool ring_grow(ring *q)
{
    int new_cap = q->cap << 1;
    pr_node **buf = malloc((size_t)new_cap * sizeof(pr_node *));
    if (!buf)
        return false;
    for (int i = 0; i < q->n; i++)
        buf[i] = *ring_at(q, i);
    free(q->buf);
    q->buf = buf;
    q->cap = new_cap;
    q->head = 0;
    return true;
}

/* Caller guarantees room. */
static void ring_push(ring *q, pr_node *v)
{
    *ring_at(q, q->n++) = v;
}

static pr_node *ring_pop(ring *q)
{
    pr_node *v = q->buf[q->head];
    q->head = (q->head + 1) & (q->cap - 1);
    q->n--;
    return v;
}

/* Remove the newest count nodes from the ring into out[], oldest of them first. */
static void ring_take_tail(ring *q, pr_node **out, int count)
{
    q->n -= count;
    for (int i = 0; i < count; i++)
        out[i] = *ring_at(q, q->n + i);
}

/* Settle node's residual and push it to the neighbours. A neighbour whose residual crosses epsilon
 * and is not already queued is claimed (in_worklist) and handed to activate().
 */
static void process_node(pr_node *node, double alpha, double epsilon,
                         void (*activate)(pr_node *, void *), void *arg)
{
    atomic_store(&node->in_worklist, false);

    /* Check before taking: a residual below epsilon must stay on the node, not be discarded. */
    if (fabs(atomic_load(&node->r)) < epsilon)
        return;
    double r = atomic_exchange(&node->r, 0.0);

    atomic_add_double(&node->p, (1.0 - alpha) * r);

    if (!node->out_degree)
        return;

    double total_w = total_out_weight(node);
    if (total_w <= 0.0)
        return;

    for (int i = 0; i < node->out_degree; i++) {
        pr_edge *e = &node->out[i];
        pr_node *neighbor = e->to;

        atomic_add_double(&neighbor->r, alpha * r * (e->weight / total_w));

        bool expected = false;
        if (fabs(atomic_load(&neighbor->r)) >= epsilon &&
            atomic_compare_exchange_strong(&neighbor->in_worklist, &expected, true))
            activate(neighbor, arg);
    }
}

/* A full computation starts over from v: all of it residual, nothing settled, every node queued. */
static void reset_to_opens(pr_node *node)
{
    atomic_store(&node->r, node->opens);
    atomic_store(&node->p, 0.0);
    atomic_store(&node->in_worklist, true);
}

/* Serial */

static void serial_activate(pr_node *node, void *arg)
{
    ring *q = arg;
    if (q->n == q->cap && !ring_grow(q)) {
        atomic_store(&node->in_worklist, false);   /* stays active on its residual */
        return;
    }
    ring_push(q, node);
}

void pr_compute_serial(pr_graph *g, double alpha, double epsilon)
{
    int n = g->n;
    if (!n) return;

    ring q;
    if (!ring_init(&q, n))
        return;

    for (int i = 0; i < n; i++) {
        reset_to_opens(pr_get(g, i));
        ring_push(&q, pr_get(g, i));
    }

    while (q.n > 0)
        process_node(ring_pop(&q), alpha, epsilon, serial_activate, &q);

    ring_free(&q);
}

/* Parallel (thread pool) */

typedef struct pr_run {
    threadpool *tp;
    _Atomic bool submit_failed;
    double alpha;
    double epsilon;
} pr_run;

typedef struct pr_batch {
    pr_run *run;
    int n;
    pr_node *nodes[PR_BATCH];
} pr_batch;

static void batch_task(void *arg);

/* Hands a batch to the pool. Nodes in it are already claimed; on failure the claims are released
 * and submit_failed is raised so the caller can fall back to the serial computation.
 */
static void batch_submit(pr_batch *b)
{
    if (b->n && !tp_submit(b->run->tp, batch_task, b))
        return;
    for (int i = 0; i < b->n; i++)
        atomic_store(&b->nodes[i]->in_worklist, false);
    if (b->n)
        atomic_store(&b->run->submit_failed, true);
    free(b);
}

static pr_batch *batch_new(pr_run *run)
{
    pr_batch *b = malloc(sizeof(*b));
    if (b) {
        b->run = run;
        b->n = 0;
    }
    return b;
}

typedef struct task_state {
    pr_run *run;
    ring q;
} task_state;

static void parallel_activate(pr_node *node, void *arg)
{
    task_state *ts = arg;
    ring *q = &ts->q;

    if (q->n == q->cap) {
        pr_batch *spill = batch_new(ts->run);
        if (!spill) {
            atomic_store(&node->in_worklist, false);
            atomic_store(&ts->run->submit_failed, true);
            return;
        }
        ring_take_tail(q, spill->nodes, PR_BATCH);
        spill->n = PR_BATCH;
        batch_submit(spill);
    }
    ring_push(q, node);
}

static void batch_task(void *arg)
{
    pr_batch *b = arg;
    task_state ts = {.run = b->run};

    if (!ring_init(&ts.q, 2 * PR_BATCH)) {
        for (int i = 0; i < b->n; i++)
            atomic_store(&b->nodes[i]->in_worklist, false);
        atomic_store(&b->run->submit_failed, true);
        free(b);
        return;
    }
    for (int i = 0; i < b->n; i++)
        ring_push(&ts.q, b->nodes[i]);
    free(b);

    while (ts.q.n > 0)
        process_node(ring_pop(&ts.q), ts.run->alpha, ts.run->epsilon,
                     parallel_activate, &ts);

    ring_free(&ts.q);
}

/* Claim node and add it to *cur, submitting *cur once full. Returns false on allocation failure. */
static bool batch_add(pr_run *run, pr_batch **cur, pr_node *node)
{
    if (!*cur) {
        *cur = batch_new(run);
        if (!*cur) {
            atomic_store(&node->in_worklist, false);
            atomic_store(&run->submit_failed, true);
            return false;
        }
    }
    (*cur)->nodes[(*cur)->n++] = node;
    if ((*cur)->n == PR_BATCH) {
        batch_submit(*cur);
        *cur = NULL;
    }
    return true;
}

static void batch_flush(pr_batch **cur)
{
    if (*cur) {
        batch_submit(*cur);
        *cur = NULL;
    }
}

void pr_compute_parallel(pr_graph *g, double alpha, double epsilon,
                         threadpool *tp)
{
    int n = g->n;
    if (!n) return;

    if (!tp) {
        pr_compute_serial(g, alpha, epsilon);
        return;
    }

    for (int i = 0; i < n; i++)
        reset_to_opens(pr_get(g, i));

    pr_run run = {.tp = tp, .submit_failed = false, .alpha = alpha, .epsilon = epsilon};
    pr_batch *cur = NULL;
    for (int i = 0; i < n; i++) {
        if (!batch_add(&run, &cur, pr_get(g, i)))
            break;
    }
    batch_flush(&cur);

    tp_wait(tp);

    if (atomic_load(&run.submit_failed))
        pr_compute_serial(g, alpha, epsilon);
}

void pr_compute_incremental(pr_graph *g, pr_node_update *updates, int n_updates,
                            double alpha, double epsilon, threadpool *tp)
{
    if (!n_updates || !g->n)
        return;

    /* Phase 1a: opens. v_i grew by k, which is the same thing as k more residual on i (the solution
     * is linear in v); it is pushed out below. Nobody else is affected.
     */
    for (int i = 0; i < n_updates; i++) {
        if (updates[i].opens > 0.0)
            atomic_add_double(&updates[i].from->r, updates[i].opens);
    }

    /* Phase 1b: inject redistribution deltas into neighbors' residuals */
    for (int i = 0; i < n_updates; i++) {
        pr_node_update *update = &updates[i];
        pr_node *node = update->from;

        double p_u = atomic_load(&node->p);
        if (p_u <= 0.0) continue;

        double coeff = (alpha / (1.0 - alpha)) * p_u;

        double amount_sum = 0.0;
        for (int j = 0; j < update->n; j++)
            amount_sum += update->deltas[j].amount;

        double new_total = total_out_weight(node);
        double old_total = new_total - amount_sum;

        if (old_total <= 0.0 && new_total <= 0.0)
            continue;

        if (old_total <= 0.0) {
            for (int j = 0; j < node->out_degree; j++) {
                double delta = coeff * (node->out[j].weight / new_total);
                atomic_add_double(&node->out[j].to->r, delta);
            }
            continue;
        }

        if (new_total <= 0.0)
            continue;

        double inv_diff = 1.0 / new_total - 1.0 / old_total;
        for (int j = 0; j < node->out_degree; j++) {
            pr_edge *e = &node->out[j];
            double delta = coeff * e->weight * inv_diff;
            if (fabs(delta) > 0.0)
                atomic_add_double(&e->to->r, delta);
        }

        double extra_coeff = coeff / old_total;
        for (int k = 0; k < update->n; k++) {
            double delta = extra_coeff * update->deltas[k].amount;
            if (fabs(delta) > 0.0)
                atomic_add_double(&update->deltas[k].to->r, delta);
        }
    }

    /* Phase 2: process nodes with significant residuals */
    if (!tp) {
        pr_compute_serial(g, alpha, epsilon);
        return;
    }

    pr_run run = {.tp = tp, .submit_failed = false, .alpha = alpha, .epsilon = epsilon};
    pr_batch *cur = NULL;
    for (int i = 0; i < g->n; i++) {
        pr_node *node = pr_get(g, i);
        if (fabs(atomic_load(&node->r)) < epsilon)
            continue;
        bool expected = false;
        if (atomic_compare_exchange_strong(&node->in_worklist, &expected, true) &&
            !batch_add(&run, &cur, node))
            break;
    }
    batch_flush(&cur);

    tp_wait(tp);

    if (atomic_load(&run.submit_failed))
        pr_compute_serial(g, alpha, epsilon);
}
