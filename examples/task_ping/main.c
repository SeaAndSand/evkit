/**
 * @file    main.c
 * @brief   ev_task 示例：同一套 work/done，后端可选 work 或 pool
 *
 *   xmake run task_ping           # 默认 ev_work
 *   xmake run task_ping -- pool   # 改用 ev_pool
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"
#include "evkit/ev_pool.h"
#include "evkit/ev_task.h"
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
    ev_pool_t *pool;
    int use_pool;
    ev_fsm_t fsm;
    int answer;
} app_t;

static void app_post(app_t *app, int event)
{
    if (ev_fsm_post(&app->fsm, event, NULL) != 0) {
        fprintf(stderr, "[main] fsm overflow\n");
    }
}

static void compute_work(ev_task_t *task)
{
    int *out = malloc(sizeof(int));
    int input = 21;
    printf("[task] compute input=%d ...\n", input);
    usleep(50 * 1000);
    if (out == NULL) {
        ev_task_return_ptr(task, NULL, -1);
        return;
    }
    *out = input * 2 + 1;
    ev_task_return_ptr(task, out, 0);
}

static void on_task_done(ev_task_t *task)
{
    app_t *app = ev_task_get_user(task);
    int status = ev_task_get_status(task);
    int *out = ev_task_get_ptr(task);

    if (status != 0 || out == NULL) {
        fprintf(stderr, "[main] ← fail status=%d\n", status);
        free(out);
        app_post(app, EV_RESULT_FAIL);
        return;
    }
    printf("[main] ← done output=%d\n", *out);
    app->answer = *out;
    free(out);
    app_post(app, EV_RESULT_OK);
}

static void action_start(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;

    ev_task_t *task = ev_task_new(app->loop, on_task_done, app, NULL);
    if (task == NULL) {
        app_post(app, EV_RESULT_FAIL);
        return;
    }

    int rc;
    if (app->use_pool) {
        printf("[main] backend=pool\n");
        rc = ev_task_run_pool(task, app->pool, compute_work);
    } else {
        printf("[main] backend=work\n");
        rc = ev_task_run_work(task, app->work, compute_work);
    }
    if (rc != 0) {
        fprintf(stderr, "[main] task run failed\n");
        ev_task_free(task);
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

int main(int argc, char **argv)
{
    int use_pool = 0;
    if (argc >= 2 && strcmp(argv[1], "pool") == 0) {
        use_pool = 1;
    }

    ev_loop_t loop;
    ev_work_t work;
    ev_pool_t pool;
    app_t app;

    memset(&app, 0, sizeof(app));
    app.loop = &loop;
    app.work = &work;
    app.pool = &pool;
    app.use_pool = use_pool;

    if (ev_loop_init(&loop) != 0) {
        return 1;
    }
    if (use_pool) {
        if (ev_pool_init(&pool, 2) != 0) {
            return 1;
        }
    } else if (ev_work_init(&work) != 0) {
        return 1;
    }

    if (ev_fsm_init(&app.fsm, ST_IDLE, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, ST_COUNT, &app, NULL) !=
        0) {
        return 1;
    }
    (void)ev_loop_register_fsm(&loop, &app.fsm);
    (void)ev_fsm_post(&app.fsm, EV_START, NULL);
    (void)ev_loop_run(&loop);

    (void)ev_loop_unregister_fsm(&loop, &app.fsm);
    ev_fsm_deinit(&app.fsm);
    if (use_pool) {
        (void)ev_pool_shutdown(&pool, EV_POOL_SHUTDOWN_DRAIN);
        ev_pool_deinit(&pool);
    } else {
        ev_work_deinit(&work);
    }
    ev_loop_deinit(&loop);

    printf("=== done answer=%d backend=%s ===\n", app.answer, use_pool ? "pool" : "work");
    return app.answer == 43 ? 0 : 1;
}
