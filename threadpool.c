#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "threadpool.h"
#include "list.h"

typedef struct work_item {
    void (*fn)(void *);
    void *arg;
    struct list_head list;
} work_item;

struct threadpool {
    pthread_t *workers;
    int n_workers;

    struct list_head queue;
    int active;            /* workers currently executing a task */
    bool shutdown;

    pthread_mutex_t lock;
    pthread_cond_t not_empty;  /* signalled when queue becomes non-empty */
    pthread_cond_t idle;       /* broadcast when active hits 0 and the queue is empty */
};

static void *worker_loop(void *arg)
{
    threadpool *tp = (threadpool *)arg;

    pthread_mutex_lock(&tp->lock);
    for (;;) {
        /* Wait for work or shutdown */
        while (list_empty(&tp->queue) && !tp->shutdown) {
            tp->active--;
            if (!tp->active)
                pthread_cond_broadcast(&tp->idle);   /* every tp_wait caller, not just one */
            pthread_cond_wait(&tp->not_empty, &tp->lock);
            tp->active++;
        }

        if (tp->shutdown && list_empty(&tp->queue)) {
            tp->active--;
            pthread_mutex_unlock(&tp->lock);
            return NULL;
        }

        /* Pop one item */
        struct list_head *node = tp->queue.next;
        list_remove(node);
        pthread_mutex_unlock(&tp->lock);

        /* Execute without holding the lock */
        work_item *item = list_entry(node, work_item, list);
        item->fn(item->arg);
        free(item);

        pthread_mutex_lock(&tp->lock);
    }
}

threadpool *tp_create(int n_workers)
{
    threadpool *tp = calloc(1, sizeof(threadpool));
    if (!tp) return NULL;

    INIT_LIST_HEAD(&tp->queue);
    tp->n_workers = n_workers;
    tp->active = n_workers;  /* all workers start as "active" (looking for work) */
    tp->shutdown = false;

    pthread_mutex_init(&tp->lock, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->idle, NULL);

    tp->workers = malloc(n_workers * sizeof(pthread_t));
    if (!tp->workers) {
        free(tp);
        return NULL;
    }

    for (int i = 0; i < n_workers; i++) {
        if (pthread_create(&tp->workers[i], NULL, worker_loop, tp)) {
            /* Roll back: signal shutdown and join already-created threads */
            pthread_mutex_lock(&tp->lock);
            tp->shutdown = true;
            pthread_cond_broadcast(&tp->not_empty);
            pthread_mutex_unlock(&tp->lock);

            for (int j = 0; j < i; j++)
                pthread_join(tp->workers[j], NULL);

            free(tp->workers);
            pthread_mutex_destroy(&tp->lock);
            pthread_cond_destroy(&tp->not_empty);
            pthread_cond_destroy(&tp->idle);
            free(tp);
            return NULL;
        }
    }

    return tp;
}

int tp_submit(threadpool *tp, void (*fn)(void *), void *arg)
{
    work_item *item = malloc(sizeof(work_item));
    if (!item) {
        fprintf(stderr, "tp_submit: malloc failed\n");
        return -1;
    }

    item->fn = fn;
    item->arg = arg;

    pthread_mutex_lock(&tp->lock);
    list_add_tail(&item->list, &tp->queue);
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->lock);
    return 0;
}

void tp_wait(threadpool *tp)
{
    pthread_mutex_lock(&tp->lock);
    while (!list_empty(&tp->queue) || tp->active > 0)
        pthread_cond_wait(&tp->idle, &tp->lock);
    pthread_mutex_unlock(&tp->lock);
}

void tp_destroy(threadpool *tp)
{
    if (!tp) return;

    /* Signal all workers to exit */
    pthread_mutex_lock(&tp->lock);
    tp->shutdown = true;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_mutex_unlock(&tp->lock);

    for (int i = 0; i < tp->n_workers; i++)
        pthread_join(tp->workers[i], NULL);

    /* Free any remaining queued items */
    struct list_head *pos, *tmp;
    list_for_each_safe(pos, tmp, &tp->queue) {
        list_remove(pos);
        work_item *item = list_entry(pos, work_item, list);
        free(item);
    }

    free(tp->workers);
    pthread_mutex_destroy(&tp->lock);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->idle);
    free(tp);
}
