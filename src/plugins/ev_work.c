/**
 * @file    ev_work.c
 * @brief   轻量 offload 插件实现
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_work.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    ev_work_done_fn done;
    void *user_data;
    ev_post_free_fn free_fn;
} ev_work_delivery_t;

typedef struct {
    ev_work_svc_t *svc;
    ev_work_fn work;
    ev_work_done_fn done;
    void *user_data;
    ev_post_free_fn free_fn;
    ev_loop_t *target_loop;
} ev_work_job_t;

static void ev_work_on_delivery(void *user_data) {
    ev_work_delivery_t *d = user_data;
    if (d->done != NULL) {
        d->done(d->user_data, 0);
    }
}

static void ev_work_free_delivery(void *user_data) {
    ev_work_delivery_t *d = user_data;
    if (d->free_fn != NULL) {
        d->free_fn(d->user_data);
    }
    free(d);
}

static void ev_work_finish_local(ev_work_job_t *job, int status) {
    if (job->done != NULL) {
        job->done(job->user_data, status);
    }
    if (job->free_fn != NULL) {
        job->free_fn(job->user_data);
    }
}

static void ev_work_job_finish(ev_work_svc_t *svc, ev_work_job_t *job) {
    if (job->target_loop != NULL && (job->done != NULL || job->free_fn != NULL)) {
        ev_work_delivery_t *d = calloc(1, sizeof(*d));
        if (d != NULL) {
            d->done = job->done;
            d->user_data = job->user_data;
            d->free_fn = job->free_fn;
            if (ev_loop_post_full(job->target_loop, ev_work_on_delivery, d, ev_work_free_delivery) == 0) {
                return;
            }
            free(d);
        }
        pthread_mutex_lock(&svc->mu);
        svc->done_dropped++;
        pthread_mutex_unlock(&svc->mu);
        ev_work_finish_local(job, -1);
        return;
    }

    ev_work_finish_local(job, 0);
}

static void *ev_work_thread_main(void *arg) {
    ev_work_job_t *job = arg;
    ev_work_svc_t *svc = job->svc;

    if (job->work != NULL) {
        job->work(job->user_data);
    }
    ev_work_job_finish(svc, job);

    pthread_mutex_lock(&svc->mu);
    svc->inflight--;
    pthread_cond_broadcast(&svc->cv);
    pthread_mutex_unlock(&svc->mu);

    free(job);
    return NULL;
}

int ev_work_init(ev_work_svc_t *svc) {
    if (svc == NULL) {
        return -1;
    }
    memset(svc, 0, sizeof(*svc));
    if (pthread_mutex_init(&svc->mu, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&svc->cv, NULL) != 0) {
        pthread_mutex_destroy(&svc->mu);
        return -1;
    }
    svc->running = 1;
    return 0;
}

int ev_work_drain(ev_work_svc_t *svc) {
    if (svc == NULL) {
        return -1;
    }
    pthread_mutex_lock(&svc->mu);
    while (svc->inflight > 0) {
        pthread_cond_wait(&svc->cv, &svc->mu);
    }
    pthread_mutex_unlock(&svc->mu);
    return 0;
}

int ev_work_inflight(ev_work_svc_t *svc) {
    if (svc == NULL) {
        return 0;
    }
    pthread_mutex_lock(&svc->mu);
    int n = svc->inflight;
    pthread_mutex_unlock(&svc->mu);
    return n;
}

void ev_work_deinit(ev_work_svc_t *svc) {
    if (svc == NULL) {
        return;
    }
    pthread_mutex_lock(&svc->mu);
    svc->running = 0;
    pthread_mutex_unlock(&svc->mu);

    (void)ev_work_drain(svc);

    pthread_cond_destroy(&svc->cv);
    pthread_mutex_destroy(&svc->mu);
    memset(svc, 0, sizeof(*svc));
}

int ev_work_run(ev_work_svc_t *svc, ev_loop_t *target_loop, ev_work_fn work, ev_work_done_fn done, void *user_data, ev_post_free_fn free_fn) {
    if (svc == NULL || work == NULL) {
        return -1;
    }
    if (done != NULL && target_loop == NULL) {
        return -1;
    }

    ev_work_job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return -1;
    }
    job->svc = svc;
    job->work = work;
    job->done = done;
    job->user_data = user_data;
    job->free_fn = free_fn;
    job->target_loop = target_loop;

    pthread_mutex_lock(&svc->mu);
    if (!svc->running) {
        pthread_mutex_unlock(&svc->mu);
        free(job);
        return -1;
    }
    svc->inflight++;
    pthread_mutex_unlock(&svc->mu);

    pthread_t tid;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        pthread_mutex_lock(&svc->mu);
        svc->inflight--;
        pthread_cond_broadcast(&svc->cv);
        pthread_mutex_unlock(&svc->mu);
        free(job);
        return -1;
    }
    (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&tid, &attr, ev_work_thread_main, job);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        pthread_mutex_lock(&svc->mu);
        svc->inflight--;
        pthread_cond_broadcast(&svc->cv);
        pthread_mutex_unlock(&svc->mu);
        free(job);
        return -1;
    }
    return 0;
}
