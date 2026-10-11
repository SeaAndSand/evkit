/**
 * @file    main.c
 * @brief   主线程编排 + 双工作线程：用 ev_loop_post 做跨线程 invoke / 结果回传
 *
 * 流程:
 *   编排器 ──post──► 线程 A（步骤1）──post 结果──► 编排器
 *   编排器根据结果决定下一步
 *   编排器 ──post──► 线程 B（步骤2）──post 结果──► 编排器
 *   编排器根据结果结束或失败
 *
 * 要点: 没有同步 RPC；「回传」= 再 post 一次到编排器 loop，在回调里推进 FSM。
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* -------------------------------------------------------------------------- */
/* 编排器状态机                                                                */
/* -------------------------------------------------------------------------- */

enum {
    ST_IDLE = 0,
    ST_WAIT_A, /* 等步骤1 */
    ST_WAIT_B, /* 等步骤2 */
    ST_DONE,
    ST_FAILED,
    ST_COUNT
};

enum {
    EV_START = 1,
    EV_A_OK,
    EV_A_FAIL,
    EV_B_OK,
    EV_B_FAIL
};

typedef struct worker worker_t;

typedef struct {
    ev_loop_t *loop;
    ev_fsm_t fsm;
    worker_t *worker_a;
    worker_t *worker_b;
    int step1_value; /* A 回传的中间结果，交给 B */
    int final_value; /* B 回传的最终结果 */
} orch_t;

/* -------------------------------------------------------------------------- */
/* 工作线程：独立 loop，只经 post 收任务 / 不直接碰编排器 FSM                    */
/* -------------------------------------------------------------------------- */

struct worker {
    const char *name;
    ev_loop_t loop;
    pthread_t tid;
    int running;
};

typedef struct {
    orch_t *orch;
    worker_t *worker;
    int input;
    int (*work_fn)(int input); /* 在工作线程跑的纯计算 */
    void (*on_result)(void *user_data); /* 投回编排器 loop 的回调 */
} job_t;

typedef struct {
    orch_t *orch;
    int ok;
    int value;
} result_t;

static void orch_post_event(orch_t *o, int event, void *arg)
{
    if (ev_fsm_post(&o->fsm, event, arg) != 0) {
        fprintf(stderr, "[orch] fsm overflow event=%d\n", event);
    }
}

/* 编排器 loop 上执行：把工作线程结果转成 FSM 事件 */
static void orch_on_a_result(void *user_data)
{
    result_t *r = user_data;
    orch_t *o = r->orch;
    printf("[orch] ← A 回传 ok=%d value=%d\n", r->ok, r->value);
    if (r->ok) {
        o->step1_value = r->value;
        orch_post_event(o, EV_A_OK, NULL);
    } else {
        orch_post_event(o, EV_A_FAIL, NULL);
    }
    free(r);
}

static void orch_on_b_result(void *user_data)
{
    result_t *r = user_data;
    orch_t *o = r->orch;
    printf("[orch] ← B 回传 ok=%d value=%d\n", r->ok, r->value);
    if (r->ok) {
        o->final_value = r->value;
        orch_post_event(o, EV_B_OK, NULL);
    } else {
        orch_post_event(o, EV_B_FAIL, NULL);
    }
    free(r);
}

/** 工作线程：执行步骤，再 invoke 回编排器 */
static void worker_on_job(void *user_data)
{
    job_t *job = user_data;
    orch_t *o = job->orch;
    const char *name = job->worker->name;

    printf("[%s] 收到任务 input=%d\n", name, job->input);
    usleep(80 * 1000); /* 模拟耗时，突出「不在编排器线程算」 */
    int value = job->work_fn(job->input);
    int ok = (value >= 0);

    result_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        free(job);
        return;
    }
    r->orch = o;
    r->ok = ok;
    r->value = value;

    /* 结果回传 = post 到编排器 loop；回调在主线程执行 */
    printf("[%s] 完成 → post 回编排器\n", name);
    if (ev_loop_post(o->loop, job->on_result, r) != 0) {
        fprintf(stderr, "[%s] post result failed\n", name);
        free(r);
    }
    free(job);
}

/** 编排器线程调用：把任务 invoke 到指定工作线程 */
static int orch_submit(worker_t *w, orch_t *o, int input, int (*work_fn)(int), void (*on_result)(void *))
{
    job_t *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return -1;
    }
    job->orch = o;
    job->worker = w;
    job->input = input;
    job->work_fn = work_fn;
    job->on_result = on_result;
    if (ev_loop_post(&w->loop, worker_on_job, job) != 0) {
        free(job);
        return -1;
    }
    return 0;
}

/* ---- 两个步骤的具体业务（跑在子线程） ---- */

static int step1_prepare(int seed)
{
    /* 步骤1：准备数据；seed>=0 成功 */
    int out = seed * 10 + 7;
    printf("[A] step1: seed=%d → %d\n", seed, out);
    return out;
}

static int step2_finish(int prepared)
{
    /* 步骤2：加工；约定 prepared<100 才成功，便于演示失败分支可改阈值 */
    int out = prepared * 2;
    printf("[B] step2: in=%d → %d\n", prepared, out);
    if (prepared < 0) {
        return -1;
    }
    return out;
}

/* ---- 编排器：按结果决定下一步 ---- */

static void orch_enter_wait_a(void *user_data, int state)
{
    (void)state;
    orch_t *o = user_data;
    printf("[orch] → WAIT_A：通知线程 A 做步骤1\n");
    if (orch_submit(o->worker_a, o, 3, step1_prepare, orch_on_a_result) != 0) {
        orch_post_event(o, EV_A_FAIL, NULL);
    }
}

static void orch_enter_wait_b(void *user_data, int state)
{
    (void)state;
    orch_t *o = user_data;
    printf("[orch] → WAIT_B：步骤1=%d，通知线程 B 做步骤2\n", o->step1_value);
    if (orch_submit(o->worker_b, o, o->step1_value, step2_finish, orch_on_b_result) != 0) {
        orch_post_event(o, EV_B_FAIL, NULL);
    }
}

static void orch_enter_done(void *user_data, int state)
{
    (void)state;
    orch_t *o = user_data;
    printf("[orch] → DONE final=%d\n", o->final_value);
    ev_loop_stop(o->loop);
}

static void orch_enter_failed(void *user_data, int state)
{
    (void)state;
    orch_t *o = user_data;
    printf("[orch] → FAILED\n");
    ev_loop_stop(o->loop);
}

static void orch_action_nop(void *user_data, int event, void *arg)
{
    (void)user_data;
    (void)event;
    (void)arg;
}

static const ev_fsm_transition_t k_orch_edges[] = {
    { ST_IDLE,    EV_START,     ST_WAIT_A, NULL },
    { ST_WAIT_A,  EV_A_OK,      ST_WAIT_B, NULL },
    { ST_WAIT_A,  EV_A_FAIL,    ST_FAILED, NULL },
    { ST_WAIT_B,  EV_B_OK,      ST_DONE,   NULL },
    { ST_WAIT_B,  EV_B_FAIL,    ST_FAILED, NULL },
    { EV_FSM_ANY, EV_FSM_FAULT, ST_FAILED, orch_action_nop },
};

static const ev_fsm_state_t k_orch_states[ST_COUNT] = {
    [ST_IDLE] =   { .name = "IDLE",   .on_enter = NULL,              .on_exit = NULL },
    [ST_WAIT_A] = { .name = "WAIT_A", .on_enter = orch_enter_wait_a, .on_exit = NULL },
    [ST_WAIT_B] = { .name = "WAIT_B", .on_enter = orch_enter_wait_b, .on_exit = NULL },
    [ST_DONE] =   { .name = "DONE",   .on_enter = orch_enter_done,   .on_exit = NULL },
    [ST_FAILED] = { .name = "FAILED", .on_enter = orch_enter_failed, .on_exit = NULL },
};

/* ---- worker 线程生命周期 ---- */

static void *worker_thread_main(void *arg)
{
    worker_t *w = arg;
    printf("[%s] loop running\n", w->name);
    (void)ev_loop_run(&w->loop);
    printf("[%s] loop exit\n", w->name);
    return NULL;
}

static int worker_start(worker_t *w, const char *name)
{
    memset(w, 0, sizeof(*w));
    w->name = name;
    if (ev_loop_init(&w->loop) != 0) {
        return -1;
    }
    if (pthread_create(&w->tid, NULL, worker_thread_main, w) != 0) {
        ev_loop_deinit(&w->loop);
        return -1;
    }
    w->running = 1;
    return 0;
}

static void worker_stop(worker_t *w)
{
    if (!w->running) {
        return;
    }
    ev_loop_stop(&w->loop);
    (void)pthread_join(w->tid, NULL);
    ev_loop_deinit(&w->loop);
    w->running = 0;
}

/* -------------------------------------------------------------------------- */

int main(void)
{
    printf("=== orchestrator demo: main loop + worker A/B via ev_loop_post ===\n");

    worker_t worker_a;
    worker_t worker_b;
    if (worker_start(&worker_a, "A") != 0 || worker_start(&worker_b, "B") != 0) {
        fprintf(stderr, "worker_start failed\n");
        return 1;
    }

    ev_loop_t orch_loop = {0};
    if (ev_loop_init(&orch_loop) != 0) {
        fprintf(stderr, "orch loop init failed\n");
        worker_stop(&worker_a);
        worker_stop(&worker_b);
        return 1;
    }

    orch_t orch = {0};
    orch.loop = &orch_loop;
    orch.worker_a = &worker_a;
    orch.worker_b = &worker_b;

    if (ev_fsm_init(&orch.fsm, ST_IDLE, k_orch_edges, sizeof(k_orch_edges) / sizeof(k_orch_edges[0]), k_orch_states, ST_COUNT, &orch, NULL) != 0) {
        fprintf(stderr, "fsm init failed\n");
        ev_loop_deinit(&orch_loop);
        worker_stop(&worker_a);
        worker_stop(&worker_b);
        return 1;
    }
    if (ev_loop_register_fsm(&orch_loop, &orch.fsm) != 0) {
        fprintf(stderr, "register_fsm failed\n");
        ev_fsm_deinit(&orch.fsm);
        ev_loop_deinit(&orch_loop);
        worker_stop(&worker_a);
        worker_stop(&worker_b);
        return 1;
    }

    /* 在编排器线程启动流水线（也可从别的线程 post 进来） */
    orch_post_event(&orch, EV_START, NULL);

    (void)ev_loop_run(&orch_loop);

    int end_state = ev_fsm_get_state(&orch.fsm);
    printf("=== exit state=%s final=%d ===\n", ev_fsm_state_name(&orch.fsm), orch.final_value);

    (void)ev_loop_unregister_fsm(&orch_loop, &orch.fsm);
    ev_fsm_deinit(&orch.fsm);
    ev_loop_deinit(&orch_loop);
    worker_stop(&worker_a);
    worker_stop(&worker_b);

    return end_state == ST_DONE ? 0 : 1;
}
