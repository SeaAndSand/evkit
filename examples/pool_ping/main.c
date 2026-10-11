/**
 * @file    main.c
 * @brief   线程池插件示例：主 loop FSM + ev_pool；done 回调里再 fsm_post
 *
 *   主线程 action ──submit──► pool workers（重活）
 *   worker 完成 ──post_full──► 主 loop done → fsm_post → DONE
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"
#include "evkit/ev_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    ST_IDLE = 0,
    ST_WAIT,
    ST_DONE,
    ST_COUNT
};

enum {
    EV_START = 1,
    EV_RESULT_OK,
    EV_RESULT_FAIL
};

typedef struct {
    ev_loop_t *loop;
    ev_pool_t *pool;
    ev_fsm_t fsm;
    int answer;
    int pending;
} app_t;

typedef struct {
    app_t *app;
    int input;
    int output;
} job_t;

static void app_post(app_t *app, int event)
{
    if (ev_fsm_post(&app->fsm, event, NULL) != 0) {
        fprintf(stderr, "[main] fsm overflow\n");
    }
}

static void job_free(void *arg)
{
    free(arg);
}

static void job_work(void *arg)
{
    job_t *job = arg;
    printf("[pool] work input=%d ...\n", job->input);
    usleep(50 * 1000);
    job->output = job->input * 2 + 1;
}

static void job_done(void *arg, int status)
{
    job_t *job = arg;
    app_t *app = job->app;
    (void)status;
    printf("[main] ← done output=%d\n", job->output);
    app->answer = job->output;
    app->pending--;
    app_post(app, EV_RESULT_OK);
    /* free 由 post_full 的 free_fn（job_free）在本回调返回后调用 */
}

static void action_start(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        app_post(app, EV_RESULT_FAIL);
        return;
    }
    job->app = app;
    job->input = 21;
    app->pending++;
    printf("[main] submit to pool\n");
    if (ev_pool_submit(app->pool, app->loop, job_work, job_done, job, job_free) != 0) {
        fprintf(stderr, "[main] submit failed\n");
        free(job);
        app->pending--;
        app_post(app, EV_RESULT_FAIL);
    }
}

static void action_done(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    printf("[main] DONE answer=%d\n", app->answer);
    ev_loop_stop(app->loop);
}

static void action_fail(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    fprintf(stderr, "[main] FAIL\n");
    ev_loop_stop(app->loop);
}

static void action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    fprintf(stderr, "[main] FAULT\n");
    ev_loop_stop(app->loop);
}

static const ev_fsm_transition_t g_tr[] = {
    { ST_IDLE, EV_START, ST_WAIT, action_start },
    { ST_WAIT, EV_RESULT_OK, ST_DONE, action_done },
    { ST_WAIT, EV_RESULT_FAIL, ST_DONE, action_fail },
    { EV_FSM_ANY, EV_FSM_FAULT, ST_DONE, action_fault },
};

static const ev_fsm_state_t g_st[ST_COUNT] = {
    [ST_IDLE] = { .name = "IDLE" },
    [ST_WAIT] = { .name = "WAIT" },
    [ST_DONE] = { .name = "DONE" },
};

static void on_boot(void *user_data)
{
    app_post(user_data, EV_START);
}

int main(void)
{
    printf("=== pool_ping: ev_pool + fsm ===\n");

    ev_loop_t loop = {0};
    ev_pool_t pool;
    if (ev_loop_init(&loop) != 0 || ev_pool_init(&pool, 2) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    app_t app = {0};
    app.loop = &loop;
    app.pool = &pool;

    if (ev_fsm_init(&app.fsm, ST_IDLE, g_tr, sizeof(g_tr) / sizeof(g_tr[0]), g_st, ST_COUNT, &app, NULL) != 0) {
        return 1;
    }
    if (ev_loop_register_fsm(&loop, &app.fsm) != 0) {
        return 1;
    }

    (void)ev_loop_post(&loop, on_boot, &app);
    (void)ev_loop_run(&loop);

    uint32_t dropped = pool.submit_dropped;
    uint32_t discarded = pool.jobs_discarded;
    (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DRAIN);
    ev_pool_deinit(&pool);
    (void)ev_loop_unregister_fsm(&loop, &app.fsm);
    ev_fsm_deinit(&app.fsm);
    ev_loop_deinit(&loop);

    printf("=== done answer=%d dropped=%u discarded=%u ===\n", app.answer, dropped, discarded);
    return (app.answer == 43 && app.pending == 0) ? 0 : 1;
}
