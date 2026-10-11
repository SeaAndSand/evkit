/**
 * @file    main.c
 * @brief   跨线程最小例：主 loop FSM + 工作线程 loop，仅用 ev_loop_post 往来
 *
 *   主线程 ──post(job)──► 工作线程（sleep/计算）
 *   工作线程 ──post(result)──► 主线程回调里 fsm_post → DONE → stop
 *
 * 要点：工作线程绝不直接碰主线程 FSM；堆 result 由主回调 free（普通 post 模型）。
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <pthread.h>
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
    ev_loop_t *main_loop;
    ev_loop_t *worker_loop;
    ev_fsm_t fsm;
    int answer;
} app_t;

typedef struct {
    app_t *app;
    int input;
} job_t;

typedef struct {
    app_t *app;
    int ok;
    int value;
} result_t;

static void app_post(app_t *app, int event, void *arg)
{
    if (ev_fsm_post(&app->fsm, event, arg) != 0) {
        fprintf(stderr, "[main] fsm overflow event=%d\n", event);
    }
}

/* 主 loop：把工作线程结果转成 FSM 事件；本回调负责 free(result) */
static void main_on_result(void *user_data)
{
    result_t *r = user_data;
    app_t *app = r->app;
    printf("[main] ← result ok=%d value=%d\n", r->ok, r->value);
    if (r->ok) {
        app->answer = r->value;
        app_post(app, EV_RESULT_OK, NULL);
    } else {
        app_post(app, EV_RESULT_FAIL, NULL);
    }
    free(r);
}

/* 工作线程：干活，再 post 回主 loop（调用者拥有 result，主回调 free） */
static void worker_on_job(void *user_data)
{
    job_t *job = user_data;
    app_t *app = job->app;
    int input = job->input;
    free(job);

    printf("[worker] job input=%d, work...\n", input);
    usleep(80 * 1000); /* 模拟重活 */
    int value = input * 2 + 1;

    result_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        fprintf(stderr, "[worker] oom\n");
        return;
    }
    r->app = app;
    r->ok = 1;
    r->value = value;
    if (ev_loop_post(app->main_loop, main_on_result, r) != 0) {
        fprintf(stderr, "[worker] post back full, free result\n");
        free(r);
    }
}

static void action_start(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        app_post(app, EV_RESULT_FAIL, NULL);
        return;
    }
    job->app = app;
    job->input = 21;
    printf("[main] → post job to worker\n");
    if (ev_loop_post(app->worker_loop, worker_on_job, job) != 0) {
        fprintf(stderr, "[main] worker post full\n");
        free(job);
        app_post(app, EV_RESULT_FAIL, NULL);
    }
}

static void action_done(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    printf("[main] DONE answer=%d, stop\n", app->answer);
    ev_loop_stop(app->main_loop);
}

static void action_fail(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    fprintf(stderr, "[main] FAIL\n");
    ev_loop_stop(app->main_loop);
}

static void action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    fprintf(stderr, "[main] FAULT hits=%u\n", app->fsm.poll_limit_hits);
    ev_loop_stop(app->main_loop);
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

static void *worker_thread(void *arg)
{
    ev_loop_t *loop = arg;
    printf("[worker] loop running\n");
    (void)ev_loop_run(loop);
    printf("[worker] loop exit\n");
    return NULL;
}

static void on_boot(void *user_data)
{
    app_t *app = user_data;
    app_post(app, EV_START, NULL);
}

int main(void)
{
    printf("=== worker_ping: cross-thread ev_loop_post ===\n");

    ev_loop_t main_loop = {0};
    ev_loop_t worker_loop = {0};
    if (ev_loop_init(&main_loop) != 0 || ev_loop_init(&worker_loop) != 0) {
        fprintf(stderr, "loop init failed\n");
        return 1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_thread, &worker_loop) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        ev_loop_deinit(&worker_loop);
        ev_loop_deinit(&main_loop);
        return 1;
    }

    app_t app = {0};
    app.main_loop = &main_loop;
    app.worker_loop = &worker_loop;

    if (ev_fsm_init(&app.fsm, ST_IDLE, g_tr, sizeof(g_tr) / sizeof(g_tr[0]), g_st, ST_COUNT, &app, NULL) != 0) {
        fprintf(stderr, "fsm init failed\n");
        return 1;
    }
    if (ev_loop_register_fsm(&main_loop, &app.fsm) != 0) {
        fprintf(stderr, "register_fsm failed\n");
        return 1;
    }

    (void)ev_loop_post(&main_loop, on_boot, &app);
    (void)ev_loop_run(&main_loop);

    ev_loop_stop(&worker_loop);
    (void)pthread_join(tid, NULL);

    (void)ev_loop_unregister_fsm(&main_loop, &app.fsm);
    ev_fsm_deinit(&app.fsm);
    ev_loop_deinit(&worker_loop);
    ev_loop_deinit(&main_loop);

    printf("=== done answer=%d ===\n", app.answer);
    return (app.answer == 43) ? 0 : 1;
}
