/**
 * @file    test_work.c
 * @brief   ev_work：run/done、drain、post 满兜底
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_loop.h"
#include "evkit/ev_work.h"
#include "test_work.h"

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
} work_ctx_t;

typedef struct {
    work_ctx_t *ctx;
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
    usleep(20 * 1000);
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

static void test_work_run_done(void)
{
    printf("[work] run + done on target_loop\n");
    ev_loop_t loop = {0};
    ev_work_svc_t svc;
    work_ctx_t ctx = {0};
    ctx.loop = &loop;

    expect_true(ev_loop_init(&loop) == 0 && ev_work_init(&svc) == 0, "init");

    job_t *job = calloc(1, sizeof(*job));
    expect_true(job != NULL, "alloc");
    job->ctx = &ctx;
    job->value = 21;

    expect_true(ev_work_run(&svc, &loop, job_work, job_done, job, job_free) == 0, "run");
    expect_true(ev_loop_run(&loop) == 0, "loop run");
    expect_true(ctx.done_n == 1, "done once");
    expect_true(ctx.last_value == 42, "value");
    expect_true(ctx.free_n == 1, "freed once");
    expect_true(ev_work_inflight(&svc) == 0, "inflight 0");

    ev_work_deinit(&svc);
    ev_loop_deinit(&loop);
}

static void slow_work(void *p)
{
    (void)p;
    usleep(80 * 1000);
}

static void test_work_drain(void)
{
    printf("[work] drain 等待在途结束\n");
    ev_work_svc_t svc;
    expect_true(ev_work_init(&svc) == 0, "init");
    expect_true(ev_work_run(&svc, NULL, slow_work, NULL, NULL, NULL) == 0, "run1");
    expect_true(ev_work_run(&svc, NULL, slow_work, NULL, NULL, NULL) == 0, "run2");
    expect_true(ev_work_inflight(&svc) >= 1, "inflight");
    expect_true(ev_work_drain(&svc) == 0, "drain");
    expect_true(ev_work_inflight(&svc) == 0, "drained");
    ev_work_deinit(&svc);
}

static void test_work_done_requires_loop(void)
{
    printf("[work] done 非 NULL 时 target_loop 必填\n");
    ev_work_svc_t svc;
    expect_true(ev_work_init(&svc) == 0, "init");
    expect_true(ev_work_run(&svc, NULL, slow_work, job_done, NULL, NULL) == -1, "reject");
    ev_work_deinit(&svc);
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

static void test_work_done_on_post_fail(void)
{
    printf("[work] target_loop post 满：临时线程 done(status!=0)+free\n");
    ev_loop_t loop = {0};
    ev_work_svc_t svc;
    g_fail_done = 0;
    g_fail_status = 0;
    g_fail_free = 0;

    expect_true(ev_loop_init(&loop) == 0 && ev_work_init(&svc) == 0, "init");
    for (int i = 0; i < EV_LOOP_POST_CAPACITY; i++) {
        expect_true(ev_loop_post(&loop, post_nop, NULL) == 0, "fill post");
    }

    void *payload = malloc(1);
    expect_true(payload != NULL, "malloc");
    expect_true(ev_work_run(&svc, &loop, fail_work, fail_done, payload, fail_free) == 0, "run");

    pthread_mutex_lock(&g_fail_mu);
    while (!g_fail_done) {
        pthread_cond_wait(&g_fail_cv, &g_fail_mu);
    }
    pthread_mutex_unlock(&g_fail_mu);

    expect_true(g_fail_status != 0, "status != 0");
    expect_true(g_fail_free == 1, "freed");
    expect_true(svc.done_dropped >= 1, "done_dropped");

    (void)ev_work_drain(&svc);
    ev_work_deinit(&svc);
    ev_loop_deinit(&loop);
}

void test_work_suite(void)
{
    printf("======== ev_work ========\n");
    test_work_run_done();
    test_work_drain();
    test_work_done_requires_loop();
    test_work_done_on_post_fail();
}
