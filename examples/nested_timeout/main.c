/**
 * @file    main.c
 * @brief   嵌套超时三场景：证明用现有 ev_loop/ev_fsm 即可写清楚
 *
 * 统一规则：
 *   1. 超时有身份（EV_DEADLINE / EV_STEP_TO / EV_Wi_TO …），禁止匿名共用
 *   2. enter arm、exit disarm；action 只做业务
 *   3. 迟到事件靠边表自然丢弃（无边则不迁）
 *
 * 场景：
 *   A. 整单 deadline + 每步超时
 *   B. 并发等待多应答，各自超时
 *   C. 父子 FSM：子超时冒泡给父
 *
 * 应答用脚本 timer 模拟；无真实网络。
 */
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <stdio.h>
#include <string.h>

/* ======================================================================== */
/* A. 整单 deadline + 每步超时                                              */
/* ======================================================================== */

enum {
    A_ST_IDLE = 0,
    A_ST_HANDSHAKE,
    A_ST_XFER,
    A_ST_DONE,
    A_ST_FAIL,
    A_ST_COUNT
};

enum {
    A_EV_START = 1,
    A_EV_ACK,
    A_EV_STEP_TO,   /* 本步超时 */
    A_EV_DEADLINE   /* 整单截止 */
};

#define A_DEADLINE_MS 200u
#define A_STEP_MS     80u
#define A_ACK_AFTER   30u /* 模拟应答早于步进超时 */

typedef struct {
    ev_loop_t *loop;
    ev_fsm_t fsm;
    ev_timer_t deadline_timer;
    ev_timer_t step_timer;
    ev_timer_t ack_timer; /* 模拟对端 ACK */
    int step;             /* 0=handshake, 1=xfer */
    int ok;
} demo_a_t;

static void a_post(demo_a_t *a, int event)
{
    if (ev_fsm_post(&a->fsm, event, NULL) != 0) {
        fprintf(stderr, "[A] fsm overflow event=%d\n", event);
    }
}

static void a_on_deadline(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    demo_a_t *a = timer->user_data;
    printf("[A] DEADLINE fired state=%s\n", ev_fsm_state_name(&a->fsm));
    a_post(a, A_EV_DEADLINE);
}

static void a_on_step_to(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    demo_a_t *a = timer->user_data;
    printf("[A] STEP_TO fired state=%s\n", ev_fsm_state_name(&a->fsm));
    a_post(a, A_EV_STEP_TO);
}

static void a_on_ack_sim(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    demo_a_t *a = timer->user_data;
    printf("[A] sim ACK\n");
    a_post(a, A_EV_ACK);
}

static void a_arm_step(demo_a_t *a)
{
    ev_timer_start(a->loop, &a->step_timer, A_STEP_MS, 0, a_on_step_to, a);
}

static void a_disarm_step(demo_a_t *a)
{
    (void)ev_timer_stop(&a->step_timer);
}

static void a_schedule_ack(demo_a_t *a)
{
    ev_timer_start(a->loop, &a->ack_timer, A_ACK_AFTER, 0, a_on_ack_sim, a);
}

static void a_enter_handshake(void *user_data, int state)
{
    (void)state;
    demo_a_t *a = user_data;
    a->step = 0;
    printf("[A] enter HANDSHAKE (step timeout %ums)\n", A_STEP_MS);
    a_arm_step(a);
    a_schedule_ack(a);
}

static void a_exit_handshake(void *user_data, int state)
{
    (void)state;
    demo_a_t *a = user_data;
    a_disarm_step(a);
    (void)ev_timer_stop(&a->ack_timer);
}

static void a_enter_xfer(void *user_data, int state)
{
    (void)state;
    demo_a_t *a = user_data;
    a->step = 1;
    printf("[A] enter XFER (step timeout %ums)\n", A_STEP_MS);
    a_arm_step(a);
    a_schedule_ack(a);
}

static void a_exit_xfer(void *user_data, int state)
{
    (void)state;
    demo_a_t *a = user_data;
    a_disarm_step(a);
    (void)ev_timer_stop(&a->ack_timer);
}

static void a_action_start(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_a_t *a = user_data;
    uint64_t dl = ev_loop_now_ms_u64() + A_DEADLINE_MS;
    printf("[A] START → arm whole-order deadline @+%ums\n", A_DEADLINE_MS);
    ev_timer_start_at(a->loop, &a->deadline_timer, dl, 0, a_on_deadline, a);
}

static void a_action_done(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_a_t *a = user_data;
    (void)ev_timer_stop(&a->deadline_timer);
    a->ok = 1;
    printf("[A] DONE\n");
    ev_loop_stop(a->loop);
}

static void a_action_fail(void *user_data, int event, void *arg)
{
    (void)arg;
    demo_a_t *a = user_data;
    (void)ev_timer_stop(&a->deadline_timer);
    a_disarm_step(a);
    (void)ev_timer_stop(&a->ack_timer);
    a->ok = 0;
    printf("[A] FAIL (event=%d)\n", event);
    ev_loop_stop(a->loop);
}

static void a_action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_a_t *a = user_data;
    a->ok = 0;
    ev_loop_stop(a->loop);
}

static const ev_fsm_transition_t a_tr[] = {
    { A_ST_IDLE, A_EV_START, A_ST_HANDSHAKE, a_action_start },
    { A_ST_HANDSHAKE, A_EV_ACK, A_ST_XFER, NULL },
    { A_ST_XFER, A_EV_ACK, A_ST_DONE, a_action_done },
    { A_ST_HANDSHAKE, A_EV_STEP_TO, A_ST_FAIL, a_action_fail },
    { A_ST_XFER, A_EV_STEP_TO, A_ST_FAIL, a_action_fail },
    { EV_FSM_ANY, A_EV_DEADLINE, A_ST_FAIL, a_action_fail },
    { EV_FSM_ANY, EV_FSM_FAULT, A_ST_FAIL, a_action_fault },
};

static const ev_fsm_state_t a_st[A_ST_COUNT] = {
    [A_ST_IDLE] = { .name = "IDLE" },
    [A_ST_HANDSHAKE] = { .name = "HANDSHAKE", .on_enter = a_enter_handshake, .on_exit = a_exit_handshake },
    [A_ST_XFER] = { .name = "XFER", .on_enter = a_enter_xfer, .on_exit = a_exit_xfer },
    [A_ST_DONE] = { .name = "DONE" },
    [A_ST_FAIL] = { .name = "FAIL" },
};

static int run_demo_a(void)
{
    printf("\n=== A: whole-order deadline + per-step timeout ===\n");

    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        return 0;
    }

    demo_a_t a = {0};
    a.loop = &loop;
    if (ev_fsm_init(&a.fsm, A_ST_IDLE, a_tr, sizeof(a_tr) / sizeof(a_tr[0]), a_st, A_ST_COUNT, &a, NULL) != 0) {
        ev_loop_deinit(&loop);
        return 0;
    }
    if (ev_loop_register_fsm(&loop, &a.fsm) != 0) {
        ev_fsm_deinit(&a.fsm);
        ev_loop_deinit(&loop);
        return 0;
    }

    a_post(&a, A_EV_START);
    (void)ev_loop_run(&loop);

    (void)ev_timer_remove(&a.deadline_timer);
    (void)ev_timer_remove(&a.step_timer);
    (void)ev_timer_remove(&a.ack_timer);
    (void)ev_loop_unregister_fsm(&loop, &a.fsm);
    ev_fsm_deinit(&a.fsm);
    ev_loop_deinit(&loop);

    printf("=== A result: %s ===\n", a.ok ? "ok" : "fail");
    return a.ok;
}

/* ======================================================================== */
/* B. 并发等待：固定 wait 槽 + 协调 FSM                                     */
/* ======================================================================== */

enum {
    B_ST_IDLE = 0,
    B_ST_WAIT_ALL,
    B_ST_DONE,
    B_ST_FAIL,
    B_ST_COUNT
};

enum {
    B_EV_START = 1,
    B_EV_W0_OK,
    B_EV_W0_TO,
    B_EV_W1_OK,
    B_EV_W1_TO,
    B_EV_W2_OK,
    B_EV_W2_TO
};

#define B_SLOT_N 3

typedef struct {
    ev_timer_t timer;
    int done; /* 1=ok, -1=timeout, 0=pending */
    int ev_ok;
    int ev_to;
    uint32_t timeout_ms;
    uint32_t reply_after_ms; /* 0 = 故意不回，靠超时 */
} wait_slot_t;

typedef struct demo_b demo_b_t;

typedef struct {
    demo_b_t *b;
    int index;
} b_slot_ctx_t;

struct demo_b {
    ev_loop_t *loop;
    ev_fsm_t fsm;
    wait_slot_t slots[B_SLOT_N];
    ev_timer_t reply_timers[B_SLOT_N]; /* 模拟应答 */
    b_slot_ctx_t to_ctx[B_SLOT_N];
    b_slot_ctx_t reply_ctx[B_SLOT_N];
    int pending;
    int ok;
};

enum { B_EV_ALL_SETTLED = 100 };

static void b_post(demo_b_t *b, int event)
{
    if (ev_fsm_post(&b->fsm, event, NULL) != 0) {
        fprintf(stderr, "[B] fsm overflow event=%d\n", event);
    }
}

static void b_on_to(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    b_slot_ctx_t *ctx = timer->user_data;
    demo_b_t *b = ctx->b;
    int i = ctx->index;
    printf("[B] slot%d TIMEOUT\n", i);
    b_post(b, b->slots[i].ev_to);
}

static void b_on_reply(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    b_slot_ctx_t *ctx = timer->user_data;
    demo_b_t *b = ctx->b;
    int i = ctx->index;
    printf("[B] slot%d OK (sim reply)\n", i);
    b_post(b, b->slots[i].ev_ok);
}

static void b_arm_all(demo_b_t *b)
{
    for (int i = 0; i < B_SLOT_N; i++) {
        wait_slot_t *s = &b->slots[i];
        s->done = 0;
        b->to_ctx[i] = (b_slot_ctx_t){ .b = b, .index = i };
        ev_timer_start(b->loop, &s->timer, s->timeout_ms, 0, b_on_to, &b->to_ctx[i]);
        if (s->reply_after_ms > 0) {
            b->reply_ctx[i] = (b_slot_ctx_t){ .b = b, .index = i };
            ev_timer_start(b->loop, &b->reply_timers[i], s->reply_after_ms, 0, b_on_reply, &b->reply_ctx[i]);
        }
    }
    b->pending = B_SLOT_N;
}

static void b_disarm_all(demo_b_t *b)
{
    for (int i = 0; i < B_SLOT_N; i++) {
        (void)ev_timer_stop(&b->slots[i].timer);
        (void)ev_timer_stop(&b->reply_timers[i]);
    }
}

static void b_mark(demo_b_t *b, int index, int ok)
{
    wait_slot_t *s = &b->slots[index];
    if (s->done != 0) {
        return; /* 迟到重复 */
    }
    s->done = ok ? 1 : -1;
    (void)ev_timer_stop(&s->timer);
    (void)ev_timer_stop(&b->reply_timers[index]);
    b->pending--;
    printf("[B] slot%d marked %s, pending=%d\n", index, ok ? "ok" : "to", b->pending);
}

static void b_check_settled(demo_b_t *b)
{
    if (b->pending > 0) {
        return;
    }
    b_post(b, B_EV_ALL_SETTLED);
}

static void b_on_slot_event(demo_b_t *b, int index, int ok)
{
    b_mark(b, index, ok);
    b_check_settled(b);
}

static void b_action_w0_ok(void *ud, int ev, void *arg)
{
    (void)ev;
    (void)arg;
    b_on_slot_event(ud, 0, 1);
}
static void b_action_w0_to(void *ud, int ev, void *arg)
{
    (void)ev;
    (void)arg;
    b_on_slot_event(ud, 0, 0);
}
static void b_action_w1_ok(void *ud, int ev, void *arg)
{
    (void)ev;
    (void)arg;
    b_on_slot_event(ud, 1, 1);
}
static void b_action_w1_to(void *ud, int ev, void *arg)
{
    (void)ev;
    (void)arg;
    b_on_slot_event(ud, 1, 0);
}
static void b_action_w2_ok(void *ud, int ev, void *arg)
{
    (void)ev;
    (void)arg;
    b_on_slot_event(ud, 2, 1);
}
static void b_action_w2_to(void *ud, int ev, void *arg)
{
    (void)ev;
    (void)arg;
    b_on_slot_event(ud, 2, 0);
}

static void b_enter_wait(void *user_data, int state)
{
    (void)state;
    demo_b_t *b = user_data;
    printf("[B] enter WAIT_ALL: arm %d slots\n", B_SLOT_N);
    b_arm_all(b);
}

static void b_exit_wait(void *user_data, int state)
{
    (void)state;
    demo_b_t *b = user_data;
    printf("[B] exit WAIT_ALL: disarm all\n");
    b_disarm_all(b);
}

static void b_action_start(void *user_data, int event, void *arg)
{
    (void)user_data;
    (void)event;
    (void)arg;
    printf("[B] START → WAIT_ALL\n");
}

static void b_action_settled(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_b_t *b = user_data;
    int ok_count = 0;
    int to_count = 0;
    for (int i = 0; i < B_SLOT_N; i++) {
        if (b->slots[i].done > 0) {
            ok_count++;
        } else {
            to_count++;
        }
    }
    /* 示范策略：允许最多 1 个超时 */
    b->ok = (ok_count >= 2 && to_count <= 1) ? 1 : 0;
    printf("[B] settled ok=%d to=%d → %s\n", ok_count, to_count, b->ok ? "pass" : "fail");
    ev_loop_stop(b->loop);
}

static void b_action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_b_t *b = user_data;
    b->ok = 0;
    ev_loop_stop(b->loop);
}

static const ev_fsm_transition_t b_tr[] = {
    { B_ST_IDLE, B_EV_START, B_ST_WAIT_ALL, b_action_start },
    { B_ST_WAIT_ALL, B_EV_W0_OK, EV_FSM_STAY, b_action_w0_ok },
    { B_ST_WAIT_ALL, B_EV_W0_TO, EV_FSM_STAY, b_action_w0_to },
    { B_ST_WAIT_ALL, B_EV_W1_OK, EV_FSM_STAY, b_action_w1_ok },
    { B_ST_WAIT_ALL, B_EV_W1_TO, EV_FSM_STAY, b_action_w1_to },
    { B_ST_WAIT_ALL, B_EV_W2_OK, EV_FSM_STAY, b_action_w2_ok },
    { B_ST_WAIT_ALL, B_EV_W2_TO, EV_FSM_STAY, b_action_w2_to },
    { B_ST_WAIT_ALL, B_EV_ALL_SETTLED, B_ST_DONE, b_action_settled },
    { EV_FSM_ANY, EV_FSM_FAULT, B_ST_FAIL, b_action_fault },
};

static const ev_fsm_state_t b_st[B_ST_COUNT] = {
    [B_ST_IDLE] = { .name = "IDLE" },
    [B_ST_WAIT_ALL] = { .name = "WAIT_ALL", .on_enter = b_enter_wait, .on_exit = b_exit_wait },
    [B_ST_DONE] = { .name = "DONE" },
    [B_ST_FAIL] = { .name = "FAIL" },
};

static int run_demo_b(void)
{
    printf("\n=== B: concurrent waits, each with own timeout ===\n");

    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        return 0;
    }

    demo_b_t b = {0};
    b.loop = &loop;
    /* slot0: 快应答；slot1: 慢应答；slot2: 不回 → 超时（允许 1 个 TO） */
    b.slots[0] = (wait_slot_t){ .ev_ok = B_EV_W0_OK, .ev_to = B_EV_W0_TO, .timeout_ms = 100, .reply_after_ms = 20 };
    b.slots[1] = (wait_slot_t){ .ev_ok = B_EV_W1_OK, .ev_to = B_EV_W1_TO, .timeout_ms = 100, .reply_after_ms = 40 };
    b.slots[2] = (wait_slot_t){ .ev_ok = B_EV_W2_OK, .ev_to = B_EV_W2_TO, .timeout_ms = 60, .reply_after_ms = 0 };

    if (ev_fsm_init(&b.fsm, B_ST_IDLE, b_tr, sizeof(b_tr) / sizeof(b_tr[0]), b_st, B_ST_COUNT, &b, NULL) != 0) {
        ev_loop_deinit(&loop);
        return 0;
    }
    if (ev_loop_register_fsm(&loop, &b.fsm) != 0) {
        ev_fsm_deinit(&b.fsm);
        ev_loop_deinit(&loop);
        return 0;
    }

    b_post(&b, B_EV_START);
    (void)ev_loop_run(&loop);

    for (int i = 0; i < B_SLOT_N; i++) {
        (void)ev_timer_remove(&b.slots[i].timer);
        (void)ev_timer_remove(&b.reply_timers[i]);
    }
    (void)ev_loop_unregister_fsm(&loop, &b.fsm);
    ev_fsm_deinit(&b.fsm);
    ev_loop_deinit(&loop);

    printf("=== B result: %s ===\n", b.ok ? "ok" : "fail");
    return b.ok;
}

/* ======================================================================== */
/* C. 父子 FSM：子超时 → 冒泡 EV_CHILD_FAIL                                 */
/* ======================================================================== */

enum {
    C_P_IDLE = 0,
    C_P_RUN,
    C_P_DONE,
    C_P_FAIL,
    C_P_COUNT
};

enum {
    C_P_EV_START = 1,
    C_P_EV_CHILD_OK,
    C_P_EV_CHILD_FAIL
};

enum {
    C_C_IDLE = 0,
    C_C_WORK,
    C_C_DONE,
    C_C_FAIL,
    C_C_COUNT
};

enum {
    C_C_EV_START = 1,
    C_C_EV_OK,
    C_C_EV_TO,
    C_C_EV_ABORT
};

#define C_CHILD_TO_MS 50u
/* 故意不发 OK，让子超时冒泡 */
#define C_CHILD_REPLY_MS 0u

typedef struct demo_c demo_c_t;

struct demo_c {
    ev_loop_t *loop;
    ev_fsm_t parent;
    ev_fsm_t child;
    ev_timer_t child_to;
    ev_timer_t child_reply;
    int ok;
};

static void c_parent_post(demo_c_t *c, int event)
{
    if (ev_fsm_post(&c->parent, event, NULL) != 0) {
        fprintf(stderr, "[C] parent overflow event=%d\n", event);
    }
}

static void c_child_post(demo_c_t *c, int event)
{
    if (ev_fsm_post(&c->child, event, NULL) != 0) {
        fprintf(stderr, "[C] child overflow event=%d\n", event);
    }
}

static void c_on_child_to(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    demo_c_t *c = timer->user_data;
    printf("[C] child STEP_TO\n");
    c_child_post(c, C_C_EV_TO);
}

static void c_on_child_reply(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    demo_c_t *c = timer->user_data;
    printf("[C] child sim OK\n");
    c_child_post(c, C_C_EV_OK);
}

static void c_child_enter_work(void *user_data, int state)
{
    (void)state;
    demo_c_t *c = user_data;
    printf("[C] child enter WORK (timeout %ums)\n", C_CHILD_TO_MS);
    ev_timer_start(c->loop, &c->child_to, C_CHILD_TO_MS, 0, c_on_child_to, c);
    if (C_CHILD_REPLY_MS > 0) {
        ev_timer_start(c->loop, &c->child_reply, C_CHILD_REPLY_MS, 0, c_on_child_reply, c);
    }
}

static void c_child_exit_work(void *user_data, int state)
{
    (void)state;
    demo_c_t *c = user_data;
    (void)ev_timer_stop(&c->child_to);
    (void)ev_timer_stop(&c->child_reply);
}

/* 同 loop 多 FSM：后 poll 的向已 poll 的 post 时，本轮不会再扫到父。
 * 空 post 唤醒下一轮，统一 poll_registered_fsms（与跨线程 post 同套路）。 */
static void c_wake_for_parent(void *user_data)
{
    (void)user_data;
}

static void c_bubble_to_parent(demo_c_t *c, int parent_event)
{
    c_parent_post(c, parent_event);
    if (ev_loop_post(c->loop, c_wake_for_parent, NULL) != 0) {
        fprintf(stderr, "[C] wake post full\n");
    }
}

static void c_child_action_done(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_c_t *c = user_data;
    printf("[C] child DONE → bubble CHILD_OK\n");
    c_bubble_to_parent(c, C_P_EV_CHILD_OK);
}

static void c_child_action_fail(void *user_data, int event, void *arg)
{
    (void)arg;
    demo_c_t *c = user_data;
    printf("[C] child FAIL (event=%d) → bubble CHILD_FAIL\n", event);
    c_bubble_to_parent(c, C_P_EV_CHILD_FAIL);
}

static void c_child_action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_c_t *c = user_data;
    c_bubble_to_parent(c, C_P_EV_CHILD_FAIL);
}

static void c_parent_enter_run(void *user_data, int state)
{
    (void)state;
    demo_c_t *c = user_data;
    printf("[C] parent enter RUN → start child\n");
    c_child_post(c, C_C_EV_START);
}

static void c_parent_exit_run(void *user_data, int state)
{
    (void)state;
    demo_c_t *c = user_data;
    /* 父离开 RUN：若子还在 WORK，发 ABORT（子 exit 会 disarm） */
    int cst = ev_fsm_get_state(&c->child);
    if (cst == C_C_WORK) {
        printf("[C] parent exit RUN → ABORT child\n");
        c_child_post(c, C_C_EV_ABORT);
    }
}

static void c_parent_action_ok(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_c_t *c = user_data;
    c->ok = 1;
    printf("[C] parent DONE\n");
    ev_loop_stop(c->loop);
}

static void c_parent_action_fail(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_c_t *c = user_data;
    c->ok = 0;
    printf("[C] parent FAIL (child bubbled)\n");
    ev_loop_stop(c->loop);
}

static void c_parent_action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    demo_c_t *c = user_data;
    c->ok = 0;
    ev_loop_stop(c->loop);
}

static const ev_fsm_transition_t c_parent_tr[] = {
    { C_P_IDLE, C_P_EV_START, C_P_RUN, NULL },
    { C_P_RUN, C_P_EV_CHILD_OK, C_P_DONE, c_parent_action_ok },
    { C_P_RUN, C_P_EV_CHILD_FAIL, C_P_FAIL, c_parent_action_fail },
    { EV_FSM_ANY, EV_FSM_FAULT, C_P_FAIL, c_parent_action_fault },
};

static const ev_fsm_state_t c_parent_st[C_P_COUNT] = {
    [C_P_IDLE] = { .name = "P_IDLE" },
    [C_P_RUN] = { .name = "P_RUN", .on_enter = c_parent_enter_run, .on_exit = c_parent_exit_run },
    [C_P_DONE] = { .name = "P_DONE" },
    [C_P_FAIL] = { .name = "P_FAIL" },
};

static const ev_fsm_transition_t c_child_tr[] = {
    { C_C_IDLE, C_C_EV_START, C_C_WORK, NULL },
    { C_C_WORK, C_C_EV_OK, C_C_DONE, c_child_action_done },
    { C_C_WORK, C_C_EV_TO, C_C_FAIL, c_child_action_fail },
    { C_C_WORK, C_C_EV_ABORT, C_C_FAIL, c_child_action_fail },
    { EV_FSM_ANY, EV_FSM_FAULT, C_C_FAIL, c_child_action_fault },
};

static const ev_fsm_state_t c_child_st[C_C_COUNT] = {
    [C_C_IDLE] = { .name = "C_IDLE" },
    [C_C_WORK] = { .name = "C_WORK", .on_enter = c_child_enter_work, .on_exit = c_child_exit_work },
    [C_C_DONE] = { .name = "C_DONE" },
    [C_C_FAIL] = { .name = "C_FAIL" },
};

static int run_demo_c(void)
{
    printf("\n=== C: parent/child FSM, child timeout bubbles up ===\n");
    printf("(child 故意不回 ACK，超时后冒泡 CHILD_FAIL — 期望 parent FAIL)\n");

    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        return 0;
    }

    demo_c_t c = {0};
    c.loop = &loop;

    if (ev_fsm_init(&c.parent, C_P_IDLE, c_parent_tr, sizeof(c_parent_tr) / sizeof(c_parent_tr[0]), c_parent_st, C_P_COUNT, &c, NULL) != 0) {
        ev_loop_deinit(&loop);
        return 0;
    }
    if (ev_fsm_init(&c.child, C_C_IDLE, c_child_tr, sizeof(c_child_tr) / sizeof(c_child_tr[0]), c_child_st, C_C_COUNT, &c, NULL) != 0) {
        ev_fsm_deinit(&c.parent);
        ev_loop_deinit(&loop);
        return 0;
    }
    if (ev_loop_register_fsm(&loop, &c.parent) != 0 || ev_loop_register_fsm(&loop, &c.child) != 0) {
        ev_fsm_deinit(&c.child);
        ev_fsm_deinit(&c.parent);
        ev_loop_deinit(&loop);
        return 0;
    }

    c_parent_post(&c, C_P_EV_START);
    (void)ev_loop_run(&loop);

    int bubbled = (c.ok == 0);
    (void)ev_timer_remove(&c.child_to);
    (void)ev_timer_remove(&c.child_reply);
    (void)ev_loop_unregister_fsm(&loop, &c.child);
    (void)ev_loop_unregister_fsm(&loop, &c.parent);
    ev_fsm_deinit(&c.child);
    ev_fsm_deinit(&c.parent);
    ev_loop_deinit(&loop);

    /* 本场景期望：子超时冒泡，父以失败结束 */
    printf("=== C result: bubbled_fail=%s ===\n", bubbled ? "yes" : "no");
    return bubbled ? 1 : 0;
}

int main(void)
{
    printf("nested_timeout: three patterns with plain ev_timer + typed events\n");

    int a = run_demo_a();
    int b = run_demo_b();
    int c = run_demo_c();

    printf("\n=== summary A=%s B=%s C=%s ===\n", a ? "pass" : "fail", b ? "pass" : "fail", c ? "pass" : "fail");
    return (a && b && c) ? 0 : 1;
}
