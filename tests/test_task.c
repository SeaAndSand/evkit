/**
 * @file    test_task.c
 * @brief   ev_task：work/pool 后端、return_ptr、run 失败释放
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_loop.h"
#include "evkit/ev_pool.h"
#include "evkit/ev_task.h"
#include "evkit/ev_work.h"
#include "test_task.h"

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
    int last_status;
    int *last_ptr_value;
    int user_freed;
} task_ctx_t;

static void ctx_free(void *p)
{
    task_ctx_t *ctx = p;
    ctx->user_freed++;
}

static void compute_work(ev_task_t *task)
{
    int *out = malloc(sizeof(int));
    if (out == NULL) {
        ev_task_return_ptr(task, NULL, -1);
        return;
    }
    *out = 21 * 2 + 1;
    usleep(10 * 1000);
    ev_task_return_ptr(task, out, 0);
}

static void on_done(ev_task_t *task)
{
    task_ctx_t *ctx = ev_task_get_user(task);
    ctx->done_n++;
    ctx->last_status = ev_task_get_status(task);
    ctx->last_ptr_value = ev_task_get_ptr(task);
    ev_loop_stop(ctx->loop);
}

static void test_task_with_work(void)
{
    printf("[task] use_work + return_ptr\n");
    ev_loop_t loop = {0};
    ev_work_svc_t work;
    task_ctx_t ctx = {0};
    ctx.loop = &loop;

    expect_true(ev_loop_init(&loop) == 0 && ev_work_init(&work) == 0, "init");
    ev_task_t *task = ev_task_new(&loop, on_done, &ctx, NULL);
    expect_true(task != NULL, "new");
    expect_true(ev_task_use_work(task, &work) == 0, "use_work");
    expect_true(ev_task_run(task, compute_work) == 0, "run");
    expect_true(ev_loop_run(&loop) == 0, "loop");
    expect_true(ctx.done_n == 1, "done");
    expect_true(ctx.last_status == 0, "status");
    expect_true(ctx.last_ptr_value != NULL && *ctx.last_ptr_value == 43, "result");
    free(ctx.last_ptr_value);

    ev_work_deinit(&work);
    ev_loop_deinit(&loop);
}

static void test_task_with_pool(void)
{
    printf("[task] use_pool + return_ptr\n");
    ev_loop_t loop = {0};
    ev_pool_t pool;
    task_ctx_t ctx = {0};
    ctx.loop = &loop;

    expect_true(ev_loop_init(&loop) == 0 && ev_pool_init(&pool, 2) == 0, "init");
    ev_task_t *task = ev_task_new(&loop, on_done, &ctx, NULL);
    expect_true(task != NULL, "new");
    expect_true(ev_task_use_pool(task, &pool) == 0, "use_pool");
    expect_true(ev_task_run(task, compute_work) == 0, "run");
    expect_true(ev_loop_run(&loop) == 0, "loop");
    expect_true(ctx.done_n == 1 && ctx.last_status == 0, "done ok");
    expect_true(ctx.last_ptr_value != NULL && *ctx.last_ptr_value == 43, "result");
    free(ctx.last_ptr_value);

    (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DRAIN);
    ev_pool_deinit(&pool);
    ev_loop_deinit(&loop);
}

static void nop_work(ev_task_t *task)
{
    (void)task;
}

static void test_task_run_requires_backend(void)
{
    printf("[task] 未选后端 run 失败，可 free\n");
    ev_loop_t loop = {0};
    task_ctx_t ctx = {0};
    expect_true(ev_loop_init(&loop) == 0, "init");
    ev_task_t *task = ev_task_new(&loop, on_done, &ctx, ctx_free);
    expect_true(task != NULL, "new");
    expect_true(ev_task_run(task, nop_work) == -1, "run fail");
    ev_task_free(task);
    expect_true(ctx.user_freed == 1, "user freed");
    ev_loop_deinit(&loop);
}

static void test_task_auto_return(void)
{
    printf("[task] work 未 return → 自动 NULL/0\n");
    ev_loop_t loop = {0};
    ev_work_svc_t work;
    task_ctx_t ctx = {0};
    ctx.loop = &loop;

    expect_true(ev_loop_init(&loop) == 0 && ev_work_init(&work) == 0, "init");
    ev_task_t *task = ev_task_new(&loop, on_done, &ctx, NULL);
    expect_true(ev_task_use_work(task, &work) == 0 && ev_task_run(task, nop_work) == 0, "run");
    expect_true(ev_loop_run(&loop) == 0, "loop");
    expect_true(ctx.done_n == 1 && ctx.last_status == 0, "status 0");
    expect_true(ctx.last_ptr_value == NULL, "ptr null");

    ev_work_deinit(&work);
    ev_loop_deinit(&loop);
}

void test_task_suite(void)
{
    printf("======== ev_task ========\n");
    test_task_with_work();
    test_task_with_pool();
    test_task_run_requires_backend();
    test_task_auto_return();
}
