/**
 * @file    ev_pool.c
 * @brief   线程池插件实现
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_pool.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    ev_pool_done_fn done;
    void *user_data;
    ev_post_free_fn free_fn;
} ev_pool_delivery_t;

static void ev_pool_on_delivery(void *user_data)
{
    ev_pool_delivery_t *d = user_data;
    if (d->done != NULL) {
        d->done(d->user_data, 0);
    }
}

static void ev_pool_free_delivery(void *user_data)
{
    ev_pool_delivery_t *d = user_data;
    if (d->free_fn != NULL) {
        d->free_fn(d->user_data);
    }
    free(d);
}

/** 投递失败或无 target_loop：在当前线程（通常为 worker）调 done/free_fn */
static void ev_pool_finish_local(ev_pool_job_t *job, int status)
{
    if (job->done != NULL) {
        job->done(job->user_data, status);
    }
    if (job->free_fn != NULL) {
        job->free_fn(job->user_data);
    }
}

static void ev_pool_job_finish(ev_pool_t *pool, ev_pool_job_t *job)
{
    if (job->target_loop != NULL && (job->done != NULL || job->free_fn != NULL)) {
        ev_pool_delivery_t *d = calloc(1, sizeof(*d));
        if (d != NULL) {
            d->done = job->done;
            d->user_data = job->user_data;
            d->free_fn = job->free_fn;
            if (ev_loop_post_full(job->target_loop, ev_pool_on_delivery, d, ev_pool_free_delivery) == 0) {
                return;
            }
            free(d);
        }
        /* OOM 或 post 满：通知调用方，避免 done 静默丢失 */
        pthread_mutex_lock(&pool->mu);
        pool->done_dropped++;
        pthread_mutex_unlock(&pool->mu);
        ev_pool_finish_local(job, -1);
        return;
    }

    ev_pool_finish_local(job, 0);
}

static void ev_pool_job_discard(ev_pool_t *pool, ev_pool_job_t *job)
{
    pool->jobs_discarded++;
    if (job->free_fn != NULL) {
        job->free_fn(job->user_data);
    }
}

static int ev_pool_queue_push_unlocked(ev_pool_t *pool, const ev_pool_job_t *job)
{
    if (pool->count >= EV_POOL_QUEUE_CAP) {
        return -1;
    }
    pool->q[pool->tail] = *job;
    pool->tail = (pool->tail + 1) % EV_POOL_QUEUE_CAP;
    pool->count++;
    return 0;
}

static int ev_pool_queue_pop_unlocked(ev_pool_t *pool, ev_pool_job_t *out)
{
    if (pool->count <= 0) {
        return -1;
    }
    *out = pool->q[pool->head];
    memset(&pool->q[pool->head], 0, sizeof(pool->q[0]));
    pool->head = (pool->head + 1) % EV_POOL_QUEUE_CAP;
    pool->count--;
    return 0;
}

static void ev_pool_discard_all_unlocked(ev_pool_t *pool)
{
    ev_pool_job_t job;
    while (ev_pool_queue_pop_unlocked(pool, &job) == 0) {
        ev_pool_job_discard(pool, &job);
    }
}

static void *ev_pool_worker_main(void *arg)
{
    ev_pool_t *pool = arg;
    for (;;) {
        ev_pool_job_t job;
        pthread_mutex_lock(&pool->mu);
        while (pool->count == 0 && !pool->stopping) {
            pthread_cond_wait(&pool->cv, &pool->mu);
        }
        if (pool->count == 0 && pool->stopping) {
            pthread_mutex_unlock(&pool->mu);
            break;
        }
        (void)ev_pool_queue_pop_unlocked(pool, &job);
        pthread_mutex_unlock(&pool->mu);

        if (job.work != NULL) {
            job.work(job.user_data);
        }
        ev_pool_job_finish(pool, &job);
    }
    return NULL;
}

int ev_pool_init(ev_pool_t *pool, int nworkers)
{
    if (pool == NULL || nworkers < 1 || nworkers > EV_POOL_WORKER_MAX) {
        return -1;
    }
    memset(pool, 0, sizeof(*pool));
    if (pthread_mutex_init(&pool->mu, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&pool->cv, NULL) != 0) {
        pthread_mutex_destroy(&pool->mu);
        return -1;
    }
    pool->nworkers = nworkers;
    pool->running = 1;
    pool->stopping = 0;

    for (int i = 0; i < nworkers; i++) {
        if (pthread_create(&pool->workers[i], NULL, ev_pool_worker_main, pool) != 0) {
            pool->running = 0;
            pool->stopping = 1;
            pthread_cond_broadcast(&pool->cv);
            for (int j = 0; j < i; j++) {
                (void)pthread_join(pool->workers[j], NULL);
            }
            pthread_cond_destroy(&pool->cv);
            pthread_mutex_destroy(&pool->mu);
            memset(pool, 0, sizeof(*pool));
            return -1;
        }
    }
    return 0;
}

int ev_pool_shutdown(ev_pool_t *pool, ev_pool_shutdown_t mode)
{
    if (pool == NULL) {
        return -1;
    }
    if (mode != EV_POOL_SHUTDOWN_DRAIN && mode != EV_POOL_SHUTDOWN_DISCARD) {
        return -1;
    }

    pthread_mutex_lock(&pool->mu);
    if (pool->nworkers <= 0) {
        pthread_mutex_unlock(&pool->mu);
        return 0;
    }
    pool->running = 0;
    pool->stopping = 1;
    if (mode == EV_POOL_SHUTDOWN_DISCARD) {
        ev_pool_discard_all_unlocked(pool);
    }
    pthread_cond_broadcast(&pool->cv);
    int n = pool->nworkers;
    pthread_mutex_unlock(&pool->mu);

    for (int i = 0; i < n; i++) {
        (void)pthread_join(pool->workers[i], NULL);
    }

    pthread_mutex_lock(&pool->mu);
    pool->nworkers = 0;
    /* DRAIN 后队列应已空；若仍有残留则丢弃释放 */
    ev_pool_discard_all_unlocked(pool);
    pthread_mutex_unlock(&pool->mu);
    return 0;
}

void ev_pool_deinit(ev_pool_t *pool)
{
    if (pool == NULL) {
        return;
    }
    if (pool->nworkers > 0) {
        (void)ev_pool_shutdown(pool, EV_POOL_SHUTDOWN_DRAIN);
    }
    pthread_cond_destroy(&pool->cv);
    pthread_mutex_destroy(&pool->mu);
    memset(pool, 0, sizeof(*pool));
}

int ev_pool_submit(ev_pool_t *pool, ev_loop_t *target_loop, ev_pool_work_fn work, ev_pool_done_fn done, void *user_data, ev_post_free_fn free_fn)
{
    if (pool == NULL || work == NULL) {
        return -1;
    }
    if (done != NULL && target_loop == NULL) {
        return -1;
    }

    ev_pool_job_t job = {
        .work = work,
        .done = done,
        .user_data = user_data,
        .free_fn = free_fn,
        .target_loop = target_loop,
    };

    pthread_mutex_lock(&pool->mu);
    if (!pool->running || pool->stopping) {
        pool->submit_dropped++;
        pthread_mutex_unlock(&pool->mu);
        return -1;
    }
    if (ev_pool_queue_push_unlocked(pool, &job) != 0) {
        pool->submit_dropped++;
        pthread_mutex_unlock(&pool->mu);
        return -1;
    }
    pthread_cond_signal(&pool->cv);
    pthread_mutex_unlock(&pool->mu);
    return 0;
}
