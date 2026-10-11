/**
 * @file    test_pool.c
 * @brief   ev_pool：submit/done、队列满、shutdown discard
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_loop.h"
#include "evkit/ev_pool.h"
#include "test_pool.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int g_test_failures;

static void expect_true(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        g_test_failures++;
    } else {
        printf("  ok %s\n", name);
    }
}

typedef struct {
    ev_loop_t *loop;
    int done_n;
    int last_value;
    int free_n;
} pool_ctx_t;

typedef struct {
    pool_ctx_t *ctx;
    int value;
} job_t;

static void job_free(void *p)
{
    job_t *j = p;
    j->ctx->free_n++;
    free(j);
}

static void job_work(void *p)
{
    job_t *j = p;
    usleep(15 * 1000);
    j->value = j->value * 2;
}

static void job_done(void *p, int status)
{
    job_t *j = p;
    (void)status;
    j->ctx->done_n++;
    j->ctx->last_value = j->value;
    if (j->ctx->done_n >= 1) {
        ev_loop_stop(j->ctx->loop);
    }
}

static void test_pool_submit_done(void)
{
    printf("[pool] submit + done on target_loop\n");
    ev_loop_t loop = {0};
    ev_pool_t pool;
    pool_ctx_t ctx = {0};
    ctx.loop = &loop;

    expect_true(ev_loop_init(&loop) == 0 && ev_pool_init(&pool, 2) == 0, "init");

    job_t *job = calloc(1, sizeof(*job));
    expect_true(job != NULL, "alloc");
    job->ctx = &ctx;
    job->value = 21;

    expect_true(ev_pool_submit(&pool, &loop, job_work, job_done, job, job_free) == 0, "submit");
    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(ctx.done_n == 1, "done once");
    expect_true(ctx.last_value == 42, "value");
    expect_true(ctx.free_n == 1, "freed once");

    (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DRAIN);
    ev_pool_deinit(&pool);
    ev_loop_deinit(&loop);
}

static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_entered;
static int g_gate_open;

static void gate_work(void *p)
{
    (void)p;
    pthread_mutex_lock(&g_gate_mu);
    g_gate_entered = 1;
    pthread_cond_signal(&g_gate_cv);
    while (!g_gate_open) {
        pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    }
    pthread_mutex_unlock(&g_gate_mu);
}

static void nop_work(void *p)
{
    (void)p;
}

static void test_pool_queue_full(void)
{
    printf("[pool] queue full → submit -1\n");
    ev_pool_t pool;
    g_gate_entered = 0;
    g_gate_open = 0;
    expect_true(ev_pool_init(&pool, 1) == 0, "init");

    expect_true(ev_pool_submit(&pool, NULL, gate_work, NULL, NULL, NULL) == 0, "busy worker");
    pthread_mutex_lock(&g_gate_mu);
    while (!g_gate_entered) {
        pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    }
    pthread_mutex_unlock(&g_gate_mu);

    int filled = 0;
    for (int i = 0; i < EV_POOL_QUEUE_CAP; i++) {
        if (ev_pool_submit(&pool, NULL, nop_work, NULL, NULL, NULL) != 0) {
            break;
        }
        filled++;
    }
    expect_true(filled == EV_POOL_QUEUE_CAP, "fill queue");
    expect_true(ev_pool_submit(&pool, NULL, nop_work, NULL, NULL, NULL) == -1, "full");
    expect_true(pool.submit_dropped >= 1, "dropped");

    pthread_mutex_lock(&g_gate_mu);
    g_gate_open = 1;
    pthread_cond_signal(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);

    (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DRAIN);
    ev_pool_deinit(&pool);
}

static int g_all_frees;

static void track_free(void *p)
{
    g_all_frees++;
    free(p);
}

static void tiny_work(void *p)
{
    (void)p;
}

static void test_pool_shutdown_discard(void)
{
    printf("[pool] shutdown DISCARD：未跑完的任务均 free\n");
    g_all_frees = 0;
    ev_pool_t pool;
    expect_true(ev_pool_init(&pool, 2) == 0, "init");

    const int n = 32;
    for (int i = 0; i < n; i++) {
        void *p = malloc(1);
        expect_true(p != NULL && ev_pool_submit(&pool, NULL, tiny_work, NULL, p, track_free) == 0, "submit");
    }

    (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DISCARD);
    expect_true(g_all_frees == n, "all freed");
    expect_true(pool.jobs_discarded + (uint32_t)(n - (int)pool.jobs_discarded) == (uint32_t)n, "accounted");
    ev_pool_deinit(&pool);
}

static void test_pool_done_requires_loop(void)
{
    printf("[pool] done 非 NULL 时 target_loop 必填\n");
    ev_pool_t pool;
    expect_true(ev_pool_init(&pool, 1) == 0, "init");
    expect_true(ev_pool_submit(&pool, NULL, tiny_work, job_done, NULL, NULL) == -1, "reject");
    (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DRAIN);
    ev_pool_deinit(&pool);
}

static pthread_mutex_t g_fail_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fail_cv = PTHREAD_COND_INITIALIZER;
static int g_fail_done;
static int g_fail_status;
static int g_fail_free;

static void fail_work(void *p)
{
    (void)p;
}

static void fail_done(void *p, int status)
{
    (void)p;
    pthread_mutex_lock(&g_fail_mu);
    g_fail_done = 1;
    g_fail_status = status;
    pthread_cond_signal(&g_fail_cv);
    pthread_mutex_unlock(&g_fail_mu);
}

static void fail_free(void *p)
{
    g_fail_free++;
    free(p);
}

static void post_nop(void *ud)
{
    (void)ud;
}

static void test_pool_done_on_post_fail(void)
{
    printf("[pool] target_loop post 满：worker 上 done(status!=0)+free\n");
    ev_loop_t loop = {0};
    ev_pool_t pool;
    g_fail_done = 0;
    g_fail_status = 0;
    g_fail_free = 0;

    expect_true(ev_loop_init(&loop) == 0 && ev_pool_init(&pool, 1) == 0, "init");
    for (int i = 0; i < EV_LOOP_POST_CAPACITY; i++) {
        expect_true(ev_loop_post(&loop, post_nop, NULL) == 0, "fill post");
    }

    void *payload = malloc(1);
    expect_true(payload != NULL, "malloc");
    expect_true(ev_pool_submit(&pool, &loop, fail_work, fail_done, payload, fail_free) == 0, "submit");

    pthread_mutex_lock(&g_fail_mu);
    while (!g_fail_done) {
        pthread_cond_wait(&g_fail_cv, &g_fail_mu);
    }
    pthread_mutex_unlock(&g_fail_mu);

    expect_true(g_fail_status != 0, "status != 0");
    expect_true(g_fail_free == 1, "freed");
    expect_true(pool.done_dropped >= 1, "done_dropped");

    (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DRAIN);
    ev_pool_deinit(&pool);
    ev_loop_deinit(&loop);
}

void test_pool_suite(void)
{
    printf("======== ev_pool ========\n");
    test_pool_submit_done();
    test_pool_queue_full();
    test_pool_shutdown_discard();
    test_pool_done_requires_loop();
    test_pool_done_on_post_fail();
}
