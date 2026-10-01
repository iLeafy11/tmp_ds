#ifndef _THREADPOOL_H
#define _THREADPOOL_H

#include <stddef.h>

typedef struct threadpool threadpool;

/**
 * Create a thread pool with the specified number of worker threads.
 * Returns NULL on failure.
 */
threadpool *tp_create(int n_workers);

/**
 * Submit a task to the thread pool. The function @fn will be called with @arg by one of the worker
 * threads. The task may itself call tp_submit() to enqueue further work (e.g. recursive directory
 * scan).
 *
 * Returns 0 on success, -1 if the task could not be queued.
 */
int tp_submit(threadpool *tp, void (*fn)(void *), void *arg);

/**
 * Block until the queue is empty AND all workers are idle. This correctly handles tasks that spawn
 * sub-tasks: it won't return prematurely just because the queue is momentarily empty while a worker
 * is still producing new tasks.
 *
 * Does not destroy the pool — you can submit more work afterwards.
 */
void tp_wait(threadpool *tp);

/**
 * Shut down the pool: signal the workers, join them, and free all resources. Workers first drain
 * whatever is still queued (a worker only leaves on shutdown when the queue is empty), so a task
 * submitted before tp_destroy still runs; submit nothing after it.
 */
void tp_destroy(threadpool *tp);

#endif /* _THREADPOOL_H */
