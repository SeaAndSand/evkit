/**
 * @file    ev_work.c
 * @brief   轻量 offload 插件实现
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_work.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    ev_work_t *work;
    ev_work_fn fn;
    ev_work_done_fn done;
    void *user_data;
    ev_post_free_fn free_fn;
    ev_loop_t *target_loop;
} ev_work_job_t;

static void ev_work_on_delivery(void *user_data)
{
    ev_work_job_t *job = user_data;
    if (job->done != NULL) {
        job->done(job->user_data, 0);
    }
}

static void ev_work_free_delivery(void *user_data)
{
    ev_work_job_t *job = user_data;
    if (job->free_fn != NULL) {
        job->free_fn(job->user_data);
    }
    free(job);
}

/** 本地完成：调 done/free_fn 并释放 job */
static void ev_work_finish_local(ev_work_job_t *job, int status)
{
    if (job->done != NULL) {
        job->done(job->user_data, status);
    }
    if (job->free_fn != NULL) {
        job->free_fn(job->user_data);
    }
    free(job);
}

/**
 * 完成后投递到 loop，或本地收尾。
 * 成功 post 后 job 由 free_delivery 释放；否则本函数释放。
 */
static void ev_work_job_finish(ev_work_t *work, ev_work_job_t *job)
{
    if (job->target_loop != NULL && (job->done != NULL || job->free_fn != NULL)) {
        if (ev_loop_post_full(job->target_loop, ev_work_on_delivery, job, ev_work_free_delivery) == 0) {
            return;
        }
        pthread_mutex_lock(&work->mu);
        work->done_dropped++;
        pthread_mutex_unlock(&work->mu);
        ev_work_finish_local(job, -1);
        return;
    }

    ev_work_finish_local(job, 0);
}

static void *ev_work_thread_main(void *arg)
{
    ev_work_job_t *job = arg;
    ev_work_t *work = job->work;

    if (job->fn != NULL) {
        job->fn(job->user_data);
    }
    ev_work_job_finish(work, job);

    pthread_mutex_lock(&work->mu);
    work->inflight--;
    pthread_cond_broadcast(&work->cv);
    pthread_mutex_unlock(&work->mu);
    return NULL;
}

static void ev_work_abort_inflight(ev_work_t *work)
{
    pthread_mutex_lock(&work->mu);
    work->inflight--;
    pthread_cond_broadcast(&work->cv);
    pthread_mutex_unlock(&work->mu);
}

int ev_work_init(ev_work_t *work)
{
    if (work == NULL) {
        return -1;
    }
    memset(work, 0, sizeof(*work));
    if (pthread_mutex_init(&work->mu, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&work->cv, NULL) != 0) {
        pthread_mutex_destroy(&work->mu);
        return -1;
    }
    work->running = 1;
    return 0;
}

int ev_work_drain(ev_work_t *work)
{
    if (work == NULL) {
        return -1;
    }
    pthread_mutex_lock(&work->mu);
    while (work->inflight > 0) {
        pthread_cond_wait(&work->cv, &work->mu);
    }
    pthread_mutex_unlock(&work->mu);
    return 0;
}

void ev_work_deinit(ev_work_t *work)
{
    if (work == NULL) {
        return;
    }
    pthread_mutex_lock(&work->mu);
    work->running = 0;
    pthread_mutex_unlock(&work->mu);

    (void)ev_work_drain(work);

    pthread_cond_destroy(&work->cv);
    pthread_mutex_destroy(&work->mu);
    memset(work, 0, sizeof(*work));
}

int ev_work_submit(ev_work_t *work, ev_loop_t *target_loop, ev_work_fn fn, ev_work_done_fn done, void *user_data,
                   ev_post_free_fn free_fn)
{
    if (work == NULL || fn == NULL) {
        return -1;
    }
    if (done != NULL && target_loop == NULL) {
        return -1;
    }

    ev_work_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return -1;
    }
    job->work = work;
    job->fn = fn;
    job->done = done;
    job->user_data = user_data;
    job->free_fn = free_fn;
    job->target_loop = target_loop;

    pthread_mutex_lock(&work->mu);
    if (!work->running) {
        pthread_mutex_unlock(&work->mu);
        free(job);
        return -1;
    }
    work->inflight++;
    pthread_mutex_unlock(&work->mu);

    pthread_t tid;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        ev_work_abort_inflight(work);
        free(job);
        return -1;
    }
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, ev_work_thread_main, job);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        ev_work_abort_inflight(work);
        free(job);
        return -1;
    }
    return 0;
}
