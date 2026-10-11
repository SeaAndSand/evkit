/**
 * @file    main.c
 * @brief   服务器整单 deadline + 步进超时失败重试（STEP ↔ RETRY）
 *
 * 场景：收到服务器命令后做多步外设交互；对端约 30s 无应答则超时。
 * 本例用缩短的毫秒演示同一结构：
 *   - deadline_timer：整单一次 start_at，贯穿重试，不因重试刷新
 *   - step_timer：仅在 STEP；RETRY 用独立 backoff timer
 *   - enter/exit 统一 arm/disarm；EV_DEADLINE 可从任意忙碌态截断
 *
 * 无 guard 时，数据相关分支用「STAY + action 内 post 细分事件」：
 *   EV_STEP_TO → EV_GO_RETRY | EV_STEP_GIVE_UP
 *   EV_ACK     → EV_NEXT_STEP | EV_ALL_DONE
 * 注意：同态 STEP→STEP 不触发 exit/enter，换步须经 ST_ADVANCE。
 *
 * 模拟：第 1 步前两次故意不回（靠步进超时重试），第 3 次才 ACK；第 2 步一次成功。
 */
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <stdio.h>
#include <string.h>

enum {
    ST_IDLE = 0,
    ST_STEP,
    ST_RETRY,
    ST_ADVANCE, /* 步间过渡：迫使 STEP→… 发生状态变化，从而跑 exit/enter */
    ST_DONE,
    ST_FAIL,
    ST_COUNT
};

enum {
    EV_CMD = 1,
    EV_ACK,
    EV_STEP_TO,
    EV_RETRY_DUE,
    EV_DEADLINE,
    /* 由 action 分发的细分事件（库无 guard） */
    EV_GO_RETRY,
    EV_STEP_GIVE_UP,
    EV_NEXT_STEP,
    EV_ALL_DONE,
    EV_RESUME /* ADVANCE → 再进 STEP */
};

#define DEADLINE_MS        500u
#define STEP_TO_MS         40u
#define RETRY_BACKOFF_MS   25u
#define STEP_TOTAL         2
#define MAX_ATTEMPT        3
#define SUCCEED_ON_ATTEMPT 3 /* 第 1 步第几次尝试才回 ACK */
#define ACK_AFTER_MS       15u

typedef struct {
    ev_loop_t *loop;
    ev_fsm_t fsm;
    ev_timer_t deadline_timer;
    ev_timer_t step_timer;
    ev_timer_t retry_timer;
    ev_timer_t ack_timer;
    int step;
    int attempt;
    unsigned gen;
    unsigned ack_gen;
    int replied;
    int ok;
} app_t;

static void app_post(app_t *app, int event)
{
    if (ev_fsm_post(&app->fsm, event, NULL) != 0) {
        fprintf(stderr, "[cmd] fsm overflow event=%d\n", event);
    }
}

static void reply_server(app_t *app, int ok, const char *why)
{
    if (app->replied) {
        return;
    }
    app->replied = 1;
    app->ok = ok;
    printf("[cmd] >> reply server: %s (%s)\n", ok ? "OK" : "FAIL", why);
}

static void on_deadline(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    app_t *app = timer->user_data;
    printf("[cmd] DEADLINE fired state=%s\n", ev_fsm_state_name(&app->fsm));
    app_post(app, EV_DEADLINE);
}

static void on_step_to(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    app_t *app = timer->user_data;
    printf("[cmd] STEP_TO step=%d attempt=%d\n", app->step, app->attempt);
    app_post(app, EV_STEP_TO);
}

static void on_retry_due(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    app_t *app = timer->user_data;
    printf("[cmd] RETRY_DUE\n");
    app_post(app, EV_RETRY_DUE);
}

static void on_ack_sim(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    app_t *app = timer->user_data;
    unsigned gen = app->ack_gen;
    printf("[cmd] sim ACK gen=%u (expect %u)\n", gen, app->gen);
    if (gen != app->gen) {
        printf("[cmd] drop late ACK (stale gen)\n");
        return;
    }
    app_post(app, EV_ACK);
}

static void schedule_ack_if_needed(app_t *app)
{
    int should_ack = (app->step > 1) || (app->attempt >= SUCCEED_ON_ATTEMPT);
    if (!should_ack) {
        printf("[cmd] step%d attempt%d: 外设故意不回（等 STEP_TO）\n", app->step, app->attempt);
        return;
    }
    app->ack_gen = app->gen;
    ev_timer_start(app->loop, &app->ack_timer, ACK_AFTER_MS, 0, on_ack_sim, app);
}

static void enter_step(void *user_data, int state)
{
    (void)state;
    app_t *app = user_data;
    app->attempt++;
    app->gen++;
    printf("[cmd] enter STEP step=%d/%d attempt=%d/%d gen=%u\n", app->step, STEP_TOTAL, app->attempt, MAX_ATTEMPT, app->gen);
    ev_timer_start(app->loop, &app->step_timer, STEP_TO_MS, 0, on_step_to, app);
    schedule_ack_if_needed(app); 
}

static void exit_step(void *user_data, int state)
{
    (void)state;
    app_t *app = user_data;
    (void)ev_timer_stop(&app->step_timer);
    (void)ev_timer_stop(&app->ack_timer);
    printf("[cmd] exit STEP (disarm step/ack)\n");
}

static void enter_retry(void *user_data, int state)
{
    (void)state;
    app_t *app = user_data;
    printf("[cmd] enter RETRY backoff=%ums (step=%d attempt=%d)\n", RETRY_BACKOFF_MS, app->step, app->attempt);
    ev_timer_start(app->loop, &app->retry_timer, RETRY_BACKOFF_MS, 0, on_retry_due, app);
}

static void exit_retry(void *user_data, int state)
{
    (void)state;
    app_t *app = user_data;
    (void)ev_timer_stop(&app->retry_timer);
    printf("[cmd] exit RETRY (disarm backoff)\n");
}

/* 同态 STEP→STEP 不会调用 exit/enter（见 ev_fsm apply_transition）；
 * 经 ADVANCE 跳一下，保证上一步 timer 被 exit 摘掉。 */
static void enter_advance(void *user_data, int state)
{
    (void)state;
    app_t *app = user_data;
    printf("[cmd] enter ADVANCE → RESUME step=%d\n", app->step);
    app_post(app, EV_RESUME);
}

static void action_cmd(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    app->step = 1;
    app->attempt = 0;
    app->gen = 0;
    app->replied = 0;
    uint64_t dl = ev_loop_now_ms_u64() + DEADLINE_MS;
    printf("[cmd] CMD → arm whole-order deadline @+%ums\n", DEADLINE_MS);
    ev_timer_start_at(app->loop, &app->deadline_timer, dl, 0, on_deadline, app);
}

static void action_ack_dispatch(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    if (app->step >= STEP_TOTAL) {
        app_post(app, EV_ALL_DONE);
        return;
    }
    app->step++;
    app->attempt = 0;
    printf("[cmd] ACK → NEXT_STEP %d\n", app->step);
    app_post(app, EV_NEXT_STEP);
}

static void action_step_to_dispatch(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    if (app->attempt < MAX_ATTEMPT) {
        printf("[cmd] STEP_TO → GO_RETRY\n");
        app_post(app, EV_GO_RETRY);
        return;
    }
    printf("[cmd] STEP_TO → GIVE_UP\n");
    app_post(app, EV_STEP_GIVE_UP);
}

static void action_all_done(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    (void)ev_timer_stop(&app->deadline_timer);
    reply_server(app, 1, "all steps ok");
    ev_loop_stop(app->loop);
}

static void action_fail(void *user_data, int event, void *arg)
{
    (void)arg;
    app_t *app = user_data;
    (void)ev_timer_stop(&app->deadline_timer);
    const char *why = (event == EV_DEADLINE) ? "whole-order deadline" : "step attempts exhausted";
    reply_server(app, 0, why);
    ev_loop_stop(app->loop);
}

static void action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    (void)ev_timer_stop(&app->deadline_timer);
    reply_server(app, 0, "FAULT");
    ev_loop_stop(app->loop);
}

static const ev_fsm_transition_t k_edges[] = {
    {.from_state = ST_IDLE,    .event = EV_CMD,          .to_state = ST_STEP,     .action = action_cmd              },

    {.from_state = ST_STEP,    .event = EV_ACK,          .to_state = EV_FSM_STAY, .action = action_ack_dispatch     },
    {.from_state = ST_STEP,    .event = EV_NEXT_STEP,    .to_state = ST_ADVANCE,  .action = NULL                    },
    {.from_state = ST_STEP,    .event = EV_ALL_DONE,     .to_state = ST_DONE,     .action = action_all_done         },

    {.from_state = ST_STEP,    .event = EV_STEP_TO,      .to_state = EV_FSM_STAY, .action = action_step_to_dispatch },
    {.from_state = ST_STEP,    .event = EV_GO_RETRY,     .to_state = ST_RETRY,    .action = NULL                    },
    {.from_state = ST_STEP,    .event = EV_STEP_GIVE_UP, .to_state = ST_FAIL,     .action = action_fail             },

    {.from_state = ST_RETRY,   .event = EV_RETRY_DUE,    .to_state = ST_STEP,     .action = NULL                    },
    {.from_state = ST_ADVANCE, .event = EV_RESUME,       .to_state = ST_STEP,     .action = NULL                    },

    {.from_state = EV_FSM_ANY, .event = EV_DEADLINE,     .to_state = ST_FAIL,     .action = action_fail             },
   {.from_state = EV_FSM_ANY, .event = EV_FSM_FAULT,    .to_state = ST_FAIL,     .action = action_fault            },
};

static const ev_fsm_state_t k_states[ST_COUNT] = {
    [ST_IDLE] =    { .name = "IDLE",    .on_enter = NULL,          .on_exit = NULL       },
    [ST_STEP] =    { .name = "STEP",    .on_enter = enter_step,    .on_exit = exit_step  },
    [ST_RETRY] =   { .name = "RETRY",   .on_enter = enter_retry,   .on_exit = exit_retry },
    [ST_ADVANCE] = { .name = "ADVANCE", .on_enter = enter_advance, .on_exit = NULL       },
    [ST_DONE] =    { .name = "DONE",    .on_enter = NULL,          .on_exit = NULL       },
    [ST_FAIL] =    { .name = "FAIL",    .on_enter = NULL,          .on_exit = NULL       },
};

int main(void)
{
    printf("=== cmd_retry: deadline + STEP/RETRY ===\n");
    printf("step1 needs %d attempts; step2 once; deadline=%ums\n\n", SUCCEED_ON_ATTEMPT, DEADLINE_MS);

    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        return 1;
    }

    app_t app = {0};
    app.loop = &loop;

    if (ev_fsm_init(&app.fsm, ST_IDLE, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, ST_COUNT, &app, NULL) != 0) {
        ev_loop_deinit(&loop);
        return 1;
    }
    if (ev_loop_register_fsm(&loop, &app.fsm) != 0) {
        ev_fsm_deinit(&app.fsm);
        ev_loop_deinit(&loop);
        return 1;
    }

    app_post(&app, EV_CMD);
    (void)ev_loop_run(&loop);

    (void)ev_timer_remove(&app.deadline_timer);
    (void)ev_timer_remove(&app.step_timer);
    (void)ev_timer_remove(&app.retry_timer);
    (void)ev_timer_remove(&app.ack_timer);
    (void)ev_loop_unregister_fsm(&loop, &app.fsm);
    ev_fsm_deinit(&app.fsm);
    ev_loop_deinit(&loop);

    printf("\n=== result: %s ===\n", app.ok ? "pass" : "fail");
    return app.ok ? 0 : 1;
}
