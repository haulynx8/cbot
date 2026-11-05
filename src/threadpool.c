#include "cbot_internal.h"

#include <stdlib.h>

typedef struct {
    cbot_task_fn fn;
    void *arg;
} ThreadPoolItem;

struct CbotThreadPool {
    pthread_t *threads;
    int num_threads;

    ThreadPoolItem *queue;
    int queue_size;
    int head;
    int tail;
    int count;

    int active_workers;
    int shutdown;

    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    pthread_cond_t drained;
};

static void *threadpool_worker(void *arg) {
    CbotThreadPool *pool = (CbotThreadPool *)arg;

    while (1) {
        pthread_mutex_lock(&pool->lock);

        while (pool->count == 0 && !pool->shutdown) {
            pthread_cond_wait(&pool->not_empty, &pool->lock);
        }

        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        ThreadPoolItem item = pool->queue[pool->head];
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;
        pool->active_workers++;

        pthread_cond_signal(&pool->not_full);
        pthread_mutex_unlock(&pool->lock);

        if (item.fn) {
            item.fn(item.arg);
        }

        pthread_mutex_lock(&pool->lock);
        pool->active_workers--;
        if (pool->count == 0 && pool->active_workers == 0) {
            pthread_cond_broadcast(&pool->drained);
        }
        pthread_mutex_unlock(&pool->lock);
    }

    return NULL;
}

CbotThreadPool *cbot_threadpool_create(int num_threads, int queue_size) {
    if (num_threads <= 0 || queue_size <= 0) return NULL;

    CbotThreadPool *pool = calloc(1, sizeof(*pool));
    if (!pool) return NULL;

    pool->num_threads = num_threads;
    pool->queue_size = queue_size;

    pool->queue = malloc((size_t)queue_size * sizeof(ThreadPoolItem));
    if (!pool->queue) {
        free(pool);
        return NULL;
    }

    pool->threads = malloc((size_t)num_threads * sizeof(pthread_t));
    if (!pool->threads) {
        free(pool->queue);
        free(pool);
        return NULL;
    }

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->not_empty, NULL);
    pthread_cond_init(&pool->not_full, NULL);
    pthread_cond_init(&pool->drained, NULL);

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&pool->threads[i], NULL, threadpool_worker, pool) != 0) {
            pthread_mutex_lock(&pool->lock);
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->not_empty);
            pthread_mutex_unlock(&pool->lock);

            while (--i >= 0) {
                pthread_join(pool->threads[i], NULL);
            }

            pthread_mutex_destroy(&pool->lock);
            pthread_cond_destroy(&pool->not_empty);
            pthread_cond_destroy(&pool->not_full);
            pthread_cond_destroy(&pool->drained);
            free(pool->threads);
            free(pool->queue);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

int cbot_threadpool_submit(CbotThreadPool *pool, cbot_task_fn fn, void *arg) {
    if (!pool || !fn) return -1;

    pthread_mutex_lock(&pool->lock);

    while (pool->count == pool->queue_size && !pool->shutdown) {
        pthread_cond_wait(&pool->not_full, &pool->lock);
    }

    if (pool->shutdown) {
        pthread_mutex_unlock(&pool->lock);
        return -1;
    }

    pool->queue[pool->tail].fn = fn;
    pool->queue[pool->tail].arg = arg;
    pool->tail = (pool->tail + 1) % pool->queue_size;
    pool->count++;

    pthread_cond_signal(&pool->not_empty);
    pthread_mutex_unlock(&pool->lock);
    return 0;
}

void cbot_threadpool_drain(CbotThreadPool *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    while (pool->count > 0 || pool->active_workers > 0) {
        pthread_cond_wait(&pool->drained, &pool->lock);
    }
    pthread_mutex_unlock(&pool->lock);
}

void cbot_threadpool_destroy(CbotThreadPool *pool) {
    if (!pool) return;

    pthread_mutex_lock(&pool->lock);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->not_empty);
    pthread_cond_broadcast(&pool->not_full);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->num_threads; i++) {
        pthread_join(pool->threads[i], NULL);
    }

    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->not_empty);
    pthread_cond_destroy(&pool->not_full);
    pthread_cond_destroy(&pool->drained);

    free(pool->threads);
    free(pool->queue);
    free(pool);
}
