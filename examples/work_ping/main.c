/**
 * @file    main.c
 * @brief   ev_work 插件示例：临时线程算完 → post 回主 loop → fsm_post
 *
 *   主线程 action ──run──► 临时线程（重活）
 *   完成 ──post_full──► 主 loop done → fsm_post → DONE
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"
#include "evkit/ev_work.h"

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
    ev_work_t *work;
    ev_fsm_t fsm;
    int answer;
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
    printf("[work] compute input=%d ...\n", job->input);
    usleep(50 * 1000);
    job->output = job->input * 2 + 1;
}

static void job_done(void *arg, int status)
{
    job_t *job = arg;
    app_t *app = job->app;
    if (status != 0) {
        fprintf(stderr, "[main] ← done deliver fail status=%d\n", status);
        app_post(app, EV_RESULT_FAIL);
        return;
    }
    printf("[main] ← done output=%d\n", job->output);
    app->answer = job->output;
    app_post(app, EV_RESULT_OK);
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
    printf("[main] ev_work_submit\n");
    if (ev_work_submit(app->work, app->loop, job_work, job_done, job, job_free) != 0) {
        fprintf(stderr, "[main] submit failed\n");
        free(job);
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

static const ev_fsm_transition_t k_edges[] = {
    { ST_IDLE, EV_START, ST_WAIT, action_start },
    { ST_WAIT, EV_RESULT_OK, ST_DONE, action_done },
    { ST_WAIT, EV_RESULT_FAIL, ST_DONE, action_fail },
    { EV_FSM_ANY, EV_FSM_FAULT, ST_DONE, action_fault },
};

static const ev_fsm_state_t k_states[ST_COUNT] = {
    [ST_IDLE] = { .name = "IDLE", .on_enter = NULL, .on_exit = NULL },
    [ST_WAIT] = { .name = "WAIT", .on_enter = NULL, .on_exit = NULL },
    [ST_DONE] = { .name = "DONE", .on_enter = NULL, .on_exit = NULL },
};

int main(void)
{
    ev_loop_t loop;
    ev_work_t work;
    app_t app;

    memset(&app, 0, sizeof(app));
    app.loop = &loop;
    app.work = &work;

    if (ev_loop_init(&loop) != 0 || ev_work_init(&work) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    if (ev_fsm_init(&app.fsm, ST_IDLE, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, ST_COUNT, &app, NULL) !=
        0) {
        fprintf(stderr, "fsm init failed\n");
        return 1;
    }
    (void)ev_loop_register_fsm(&loop, &app.fsm);
    (void)ev_fsm_post(&app.fsm, EV_START, NULL);

    (void)ev_loop_run(&loop);

    (void)ev_loop_unregister_fsm(&loop, &app.fsm);
    ev_fsm_deinit(&app.fsm);
    uint32_t dropped = work.done_dropped;
    ev_work_deinit(&work);
    ev_loop_deinit(&loop);

    printf("=== done answer=%d dropped=%u ===\n", app.answer, dropped);
    return app.answer == 43 ? 0 : 1;
}
