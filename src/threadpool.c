#include "threadpool.h"

#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

static void *worker(void *arg) {
    ThreadPool *tp = (ThreadPool *)arg;

    for (;;) {
        pthread_mutex_lock(&tp->mtx);

        /* Wait until there's work or we're shutting down */
        while (tp->count == 0 && tp->shutdown == 0)
            pthread_cond_wait(&tp->not_empty, &tp->mtx);

        /* Drain mode: finish remaining jobs then exit */
        if (tp->count == 0 && tp->shutdown) {
            pthread_mutex_unlock(&tp->mtx);
            return NULL;
        }

        Job job = tp->queue[tp->head];
        tp->head  = (tp->head + 1) % QUEUE_CAPACITY;
        tp->count--;
        pthread_cond_signal(&tp->not_full);
        pthread_mutex_unlock(&tp->mtx);

        job.fn(job.arg);
    }
}

int threadpool_init(ThreadPool *tp) {
    memset(tp, 0, sizeof(*tp));
    pthread_mutex_init(&tp->mtx, NULL);
    pthread_cond_init(&tp->not_empty, NULL);
    pthread_cond_init(&tp->not_full, NULL);

    for (int i = 0; i < THREADPOOL_SIZE; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker, tp) != 0) {
            perror("threadpool_init: pthread_create");
            /* Attempt to shut down whatever started */
            tp->shutdown = 1;
            pthread_cond_broadcast(&tp->not_empty);
            for (int j = 0; j < i; j++)
                pthread_join(tp->threads[j], NULL);
            return -1;
        }
    }
    return 0;
}

int threadpool_submit(ThreadPool *tp, job_fn_t fn, void *arg) {
    pthread_mutex_lock(&tp->mtx);

    if (tp->shutdown) {
        pthread_mutex_unlock(&tp->mtx);
        return -1;
    }

    /* Block until there is room in the queue */
    while (tp->count == QUEUE_CAPACITY && !tp->shutdown)
        pthread_cond_wait(&tp->not_full, &tp->mtx);

    if (tp->shutdown) {
        pthread_mutex_unlock(&tp->mtx);
        return -1;
    }

    tp->queue[tp->tail].fn  = fn;
    tp->queue[tp->tail].arg = arg;
    tp->tail  = (tp->tail + 1) % QUEUE_CAPACITY;
    tp->count++;
    pthread_cond_signal(&tp->not_empty);
    pthread_mutex_unlock(&tp->mtx);
    return 0;
}

void threadpool_destroy(ThreadPool *tp) {
    pthread_mutex_lock(&tp->mtx);
    tp->shutdown = 1;
    pthread_cond_broadcast(&tp->not_empty);
    pthread_cond_broadcast(&tp->not_full);
    pthread_mutex_unlock(&tp->mtx);

    for (int i = 0; i < THREADPOOL_SIZE; i++)
        pthread_join(tp->threads[i], NULL);

    pthread_mutex_destroy(&tp->mtx);
    pthread_cond_destroy(&tp->not_empty);
    pthread_cond_destroy(&tp->not_full);
}
