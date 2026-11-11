#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "protocol.h"
#include <stddef.h>
#include <pthread.h>

/*
 * Bounded thread pool backed by a fixed-size circular job queue.
 *
 * Workers sleep on a condition variable until a job is available.
 * On shutdown (threadpool_destroy), workers drain remaining jobs then exit.
 */

typedef void (*job_fn_t)(void *arg);

typedef struct {
    job_fn_t fn;
    void    *arg;
} Job;

typedef struct {
    pthread_t        threads[THREADPOOL_SIZE];
    Job              queue[QUEUE_CAPACITY];
    int              head;        /* next slot to read  */
    int              tail;        /* next slot to write */
    int              count;       /* jobs currently queued */
    int              shutdown;    /* 1 = draining, 2 = immediate */
    pthread_mutex_t  mtx;
    pthread_cond_t   not_empty;
    pthread_cond_t   not_full;
} ThreadPool;

/* Initialise the pool and start THREADPOOL_SIZE worker threads. */
int  threadpool_init(ThreadPool *tp);

/*
 * Submit a job.  Blocks if the queue is full.
 * Returns 0 on success, -1 if the pool is shutting down.
 */
int  threadpool_submit(ThreadPool *tp, job_fn_t fn, void *arg);

/*
 * Signal shutdown, drain the queue, join all workers.
 * After this call the pool must not be used.
 */
void threadpool_destroy(ThreadPool *tp);

#endif /* THREADPOOL_H */
