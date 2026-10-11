/**
 * @file    test_ownership.c
 * @brief   边界情况与内存模型契约验证（FSM free_arg / loop_post / post_full / bus）
 */
#include "evkit/ev_bus.h"
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 由 test_evkit.c 定义 */
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

/* ========================================================================== */
/* FSM                                                                        */
/* ========================================================================== */

enum { OS0 = 0, OS1, OS2, OS_COUNT };
enum { OE_GO = 1, OE_UNKNOWN = 99 };

static int g_own_freed;
static void *g_action_saw_arg;

static void own_free_arg(void *arg)
{
    if (arg == NULL) {
        return;
    }
    g_own_freed++;
    free(arg);
}

static void own_action_keep_ptr(void *ud, int event, void *arg)
{
    (void)ud;
    (void)event;
    g_action_saw_arg = arg;
}

static void own_action_deinit_self(void *ud, int event, void *arg)
{
    (void)event;
    (void)arg;
    ev_fsm_deinit((ev_fsm_t *)ud);
}

static const ev_fsm_transition_t k_own_edges[] = {
    { .from_state = OS0, .event = OE_GO, .to_state = OS1, .action = own_action_keep_ptr },
    { .from_state = OS1, .event = OE_GO, .to_state = OS1, .action = own_action_deinit_self },
    { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = OS2, .action = NULL },
};

static int g_fault_freed;

static void fault_free_arg(void *arg)
{
    if (arg == NULL) {
        return;
    }
    g_fault_freed++;
    free(arg);
}

static void on_enter_post_heap(void *ud, int state)
{
    (void)state;
    ev_fsm_t *fsm = ud;
    void *p = malloc(1);
    if (p != NULL) {
        (void)ev_fsm_post(fsm, OE_GO, p);
    } else {
        (void)ev_fsm_post(fsm, OE_GO, NULL);
    }
}

static const ev_fsm_transition_t k_fault_edges[] = {
    { .from_state = OS0, .event = OE_GO, .to_state = OS1, .action = NULL },
    { .from_state = OS1, .event = OE_GO, .to_state = OS0, .action = NULL },
    { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = OS2, .action = NULL },
};

static const ev_fsm_state_t k_fault_states[OS_COUNT] = {
    [OS0] = { .name = "0", .on_enter = on_enter_post_heap, .on_exit = NULL },
    [OS1] = { .name = "1", .on_enter = on_enter_post_heap, .on_exit = NULL },
    [OS2] = { .name = "2", .on_enter = NULL, .on_exit = NULL },
};

static void test_fsm_free_arg_on_overflow(void)
{
    printf("[own/fsm] 队列满：立刻 free_arg\n");
    ev_fsm_t fsm = {0};
    g_own_freed = 0;
    expect_true(ev_fsm_init(&fsm, OS0, k_own_edges, sizeof(k_own_edges) / sizeof(k_own_edges[0]), NULL, 0, NULL,
                            own_free_arg) == 0,
                "init");
    int filled = 1;
    for (int i = 0; i < EV_FSM_QUEUE_CAPACITY; i++) {
        if (ev_fsm_post(&fsm, OE_GO, NULL) != 0) {
            filled = 0;
            break;
        }
    }
    expect_true(filled, "fill queue");
    void *p = malloc(8);
    expect_true(p != NULL, "malloc");
    expect_true(ev_fsm_post(&fsm, OE_GO, p) == -1, "overflow");
    expect_true(g_own_freed == 1, "overflow freed arg");
    expect_true(fsm.post_dropped >= 1, "dropped");
    ev_fsm_deinit(&fsm);
}

static void test_fsm_free_arg_unmatched(void)
{
    printf("[own/fsm] 无匹配边：丢弃并 free_arg\n");
    ev_fsm_t fsm = {0};
    g_own_freed = 0;
    expect_true(ev_fsm_init(&fsm, OS0, k_own_edges, sizeof(k_own_edges) / sizeof(k_own_edges[0]), NULL, 0, NULL,
                            own_free_arg) == 0,
                "init");
    void *p = malloc(4);
    expect_true(p != NULL && ev_fsm_post(&fsm, OE_UNKNOWN, p) == 0, "post unknown");
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == OS0, "state unchanged");
    expect_true(g_own_freed == 1, "unmatched freed");
    ev_fsm_deinit(&fsm);
}

static void test_fsm_free_arg_after_action(void)
{
    printf("[own/fsm] 成功迁移后仍 free_arg（action 不可接管）\n");
    ev_fsm_t fsm = {0};
    g_own_freed = 0;
    g_action_saw_arg = NULL;
    expect_true(ev_fsm_init(&fsm, OS0, k_own_edges, sizeof(k_own_edges) / sizeof(k_own_edges[0]), NULL, 0, &fsm,
                            own_free_arg) == 0,
                "init");
    void *p = malloc(4);
    expect_true(p != NULL && ev_fsm_post(&fsm, OE_GO, p) == 0, "post");
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == OS1, "S1");
    expect_true(g_action_saw_arg == p, "action saw arg");
    expect_true(g_own_freed == 1, "freed after action");
    ev_fsm_deinit(&fsm);
}

static void test_fsm_deinit_during_poll_no_clear(void)
{
    printf("[own/fsm] poll 内 deinit：直接返回，本轮 poll 继续消化\n");
    ev_fsm_t fsm = {0};
    g_own_freed = 0;
    expect_true(ev_fsm_init(&fsm, OS1, k_own_edges, sizeof(k_own_edges) / sizeof(k_own_edges[0]), NULL, 0, &fsm,
                            own_free_arg) == 0,
                "init");
    void *a = malloc(1);
    void *b = malloc(1);
    expect_true(a != NULL && b != NULL, "malloc");
    expect_true(ev_fsm_post(&fsm, OE_GO, a) == 0, "post a");
    expect_true(ev_fsm_post(&fsm, OE_GO, b) == 0, "post b");
    ev_fsm_poll(&fsm);
    expect_true(g_own_freed == 2, "both freed by poll");
    expect_true(fsm.transitions != NULL, "tables not wiped mid-poll");
    ev_fsm_deinit(&fsm);
    expect_true(fsm.transitions == NULL, "deinit after poll ok");
}

static void test_fsm_fault_clears_queue_free_arg(void)
{
    printf("[own/fsm] FAULT/步数上限清队走 free_arg\n");
    ev_fsm_t fsm = {0};
    g_fault_freed = 0;
    expect_true(ev_fsm_init(&fsm, OS0, k_fault_edges, sizeof(k_fault_edges) / sizeof(k_fault_edges[0]), k_fault_states,
                            OS_COUNT, &fsm, fault_free_arg) == 0,
                "init");
    ev_fsm_poll(&fsm);
    expect_true(fsm.poll_limit_hits >= 1, "hit limit");
    expect_true(ev_fsm_get_state(&fsm) == OS2, "FAULT state");
    expect_true(ev_fsm_queue_len(&fsm) == 0, "queue empty");
    expect_true(g_fault_freed > 0, "heap args freed");
    ev_fsm_deinit(&fsm);
}

static void test_fsm_null_free_arg_null_payload(void)
{
    printf("[own/fsm] free_arg=NULL 且 arg=NULL 可正常 poll\n");
    ev_fsm_t fsm = {0};
    expect_true(ev_fsm_init(&fsm, OS0, k_own_edges, sizeof(k_own_edges) / sizeof(k_own_edges[0]), NULL, 0, &fsm, NULL) == 0,
                "init");
    expect_true(ev_fsm_post(&fsm, OE_GO, NULL) == 0, "post");
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == OS1, "ok");
    ev_fsm_deinit(&fsm);
}

/* ========================================================================== */
/* loop                                                                       */
/* ========================================================================== */

static int g_plain_cb;
static int g_nested_order;
static int g_nested_a;
static int g_nested_b;

static void plain_cb(void *ud)
{
    (void)ud;
    g_plain_cb++;
}

static void nested_post_b_stop(void *ud)
{
    ev_loop_t *loop = ud;
    g_nested_b = ++g_nested_order;
    ev_loop_stop(loop);
}

static void nested_post_a(void *ud)
{
    ev_loop_t *loop = ud;
    g_nested_a = ++g_nested_order;
    (void)ev_loop_post(loop, nested_post_b_stop, loop);
}

static void stop_loop(void *ud)
{
    ev_loop_stop((ev_loop_t *)ud);
}

static void test_loop_plain_post_deinit_does_not_free(void)
{
    printf("[own/loop] 普通 post：deinit 不回调、不释放（调用者 free）\n");
    ev_loop_t loop = {0};
    g_plain_cb = 0;
    expect_true(ev_loop_init(&loop) == 0, "init");
    void *p = malloc(16);
    expect_true(p != NULL, "malloc");
    expect_true(ev_loop_post(&loop, plain_cb, p) == 0, "post");
    ev_loop_deinit(&loop);
    expect_true(g_plain_cb == 0, "no callback");
    free(p);
    expect_true(1, "caller freed safely");
}

static void test_loop_post_overflow_caller_frees(void)
{
    printf("[own/loop] post 满：返回 -1，调用者 free\n");
    ev_loop_t loop = {0};
    expect_true(ev_loop_init(&loop) == 0, "init");
    int filled = 1;
    for (int i = 0; i < EV_LOOP_POST_CAPACITY; i++) {
        if (ev_loop_post(&loop, plain_cb, NULL) != 0) {
            filled = 0;
            break;
        }
    }
    expect_true(filled, "fill");
    void *p = malloc(4);
    expect_true(p != NULL, "malloc");
    expect_true(ev_loop_post(&loop, plain_cb, p) == -1, "full");
    expect_true(loop.post_queue_dropped >= 1, "dropped");
    free(p);
    ev_loop_deinit(&loop);
}

static void test_loop_nested_post_from_callback(void)
{
    printf("[own/loop] 回调内再 post\n");
    ev_loop_t loop = {0};
    g_nested_order = 0;
    g_nested_a = 0;
    g_nested_b = 0;
    expect_true(ev_loop_init(&loop) == 0, "init");
    expect_true(ev_loop_post(&loop, nested_post_a, &loop) == 0, "post a");
    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(g_nested_a == 1 && g_nested_b == 2, "A then B");
    ev_loop_deinit(&loop);
}

static int g_full_cb2;
static int g_full_free2;

static void full_cb2(void *ud)
{
    (void)ud;
    g_full_cb2++;
}

static void full_free2(void *ud)
{
    free(ud);
    g_full_free2++;
}

static void test_loop_post_full_exactly_once(void)
{
    printf("[own/loop] post_full：成功路径 free_fn 恰好一次\n");
    ev_loop_t loop = {0};
    g_full_cb2 = 0;
    g_full_free2 = 0;
    expect_true(ev_loop_init(&loop) == 0, "init");
    void *p = malloc(8);
    expect_true(p != NULL, "malloc");
    expect_true(ev_loop_post_full(&loop, full_cb2, p, full_free2) == 0, "post_full");
    expect_true(ev_loop_post(&loop, stop_loop, &loop) == 0, "stop");
    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(g_full_cb2 == 1, "cb");
    expect_true(g_full_free2 == 1, "free once");
    ev_loop_deinit(&loop);
}

static void test_loop_post_full_fail_no_free_fn(void)
{
    printf("[own/loop] post_full 满：不调 free_fn，调用者 free\n");
    ev_loop_t loop = {0};
    g_full_free2 = 0;
    expect_true(ev_loop_init(&loop) == 0, "init");
    for (int i = 0; i < EV_LOOP_POST_CAPACITY; i++) {
        (void)ev_loop_post(&loop, plain_cb, NULL);
    }
    void *p = malloc(4);
    expect_true(p != NULL, "malloc");
    expect_true(ev_loop_post_full(&loop, full_cb2, p, full_free2) == -1, "full");
    expect_true(g_full_free2 == 0, "free_fn not called");
    free(p);
    ev_loop_deinit(&loop);
}

static void test_loop_register_fsm_max(void)
{
    printf("[own/loop] register_fsm 满返回 -1\n");
    ev_loop_t loop = {0};
    expect_true(ev_loop_init(&loop) == 0, "init");
    ev_fsm_t fsms[EV_LOOP_FSM_MAX + 1];
    static const ev_fsm_transition_t edges[] = {
        { .from_state = 0, .event = EV_FSM_FAULT, .to_state = 0, .action = NULL },
    };
    int ok = 1;
    for (int i = 0; i < EV_LOOP_FSM_MAX; i++) {
        if (ev_fsm_init(&fsms[i], 0, edges, 1, NULL, 0, NULL, NULL) != 0 ||
            ev_loop_register_fsm(&loop, &fsms[i]) != 0) {
            ok = 0;
            break;
        }
    }
    expect_true(ok, "fill fsm slots");
    expect_true(ev_fsm_init(&fsms[EV_LOOP_FSM_MAX], 0, edges, 1, NULL, 0, NULL, NULL) == 0, "extra init");
    expect_true(ev_loop_register_fsm(&loop, &fsms[EV_LOOP_FSM_MAX]) == -1, "full");
    for (int i = 0; i < EV_LOOP_FSM_MAX; i++) {
        (void)ev_loop_unregister_fsm(&loop, &fsms[i]);
        ev_fsm_deinit(&fsms[i]);
    }
    ev_fsm_deinit(&fsms[EV_LOOP_FSM_MAX]);
    ev_loop_deinit(&loop);
}

/* ========================================================================== */
/* bus                                                                        */
/* ========================================================================== */

static int g_sync_n;
static int g_bus_post_cb;
static int *g_stack_seen;

static void bus_sync_count(const void *topic, void *payload, void *ud)
{
    (void)topic;
    (void)payload;
    (void)ud;
    g_sync_n++;
}

static void bus_post_nop(const void *topic, void *payload, void *ud)
{
    (void)topic;
    (void)payload;
    (void)ud;
    g_bus_post_cb++;
}

static void bus_on_stack_payload(const void *topic, void *payload, void *ud)
{
    (void)topic;
    (void)ud;
    if (payload != NULL && g_stack_seen != NULL) {
        *g_stack_seen = *(int *)payload;
    }
}

static int match_always(const void *topic, const void *match_arg, void *ud)
{
    (void)topic;
    (void)match_arg;
    (void)ud;
    return 1;
}

static void test_bus_publish_no_subscribers(void)
{
    printf("[own/bus] 无订阅 publish 返回 0\n");
    ev_loop_t loop = {0};
    ev_bus_t bus;
    static uint32_t topic = 1;
    expect_true(ev_loop_init(&loop) == 0 && ev_bus_init(&bus, &loop) == 0, "init");
    expect_true(ev_bus_publish(&bus, &topic, NULL) == 0, "zero");
    ev_bus_deinit(&bus);
    ev_loop_deinit(&loop);
}

static void test_bus_multi_sync_same_topic(void)
{
    printf("[own/bus] 同 topic 多个 SYNC 均触发\n");
    ev_loop_t loop = {0};
    ev_bus_t bus;
    static uint32_t topic = 5;
    g_sync_n = 0;
    expect_true(ev_loop_init(&loop) == 0 && ev_bus_init(&bus, &loop) == 0, "init");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_sync_count, NULL, ev_bus_match_u32, &topic) > 0, "s1");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_sync_count, NULL, ev_bus_match_u32, &topic) > 0, "s2");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_sync_count, NULL, ev_bus_match_u32, &topic) > 0, "s3");
    expect_true(ev_bus_publish(&bus, &topic, NULL) == 3, "delivered 3");
    expect_true(g_sync_n == 3, "all sync");
    ev_bus_deinit(&bus);
    ev_loop_deinit(&loop);
}

static void test_bus_sub_max(void)
{
    printf("[own/bus] 订阅满返回 -1\n");
    ev_loop_t loop = {0};
    ev_bus_t bus;
    static uint32_t topic = 1;
    expect_true(ev_loop_init(&loop) == 0 && ev_bus_init(&bus, &loop) == 0, "init");
    int ok = 1;
    for (int i = 0; i < EV_BUS_SUB_MAX; i++) {
        if (ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_sync_count, NULL, ev_bus_match_u32, &topic) <= 0) {
            ok = 0;
            break;
        }
    }
    expect_true(ok, "fill");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_sync_count, NULL, ev_bus_match_u32, &topic) == -1, "full");
    ev_bus_deinit(&bus);
    ev_loop_deinit(&loop);
}

static void test_bus_sync_stack_payload_ok(void)
{
    printf("[own/bus] SYNC 可用栈上 payload\n");
    ev_loop_t loop = {0};
    ev_bus_t bus;
    static uint32_t topic = 7;
    int seen = -1;
    g_stack_seen = &seen;
    expect_true(ev_loop_init(&loop) == 0 && ev_bus_init(&bus, &loop) == 0, "init");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_on_stack_payload, NULL, ev_bus_match_u32, &topic) > 0, "sub");
    int stack_val = 12345;
    expect_true(ev_bus_publish(&bus, &topic, &stack_val) == 1, "pub");
    expect_true(seen == 12345, "got stack payload");
    ev_bus_deinit(&bus);
    ev_loop_deinit(&loop);
}

static void test_bus_post_target_deinit_frees_delivery(void)
{
    printf("[own/bus] POST 入队后目标 loop deinit：delivery free_fn（cb 不跑）\n");
    ev_loop_t owner = {0};
    ev_loop_t worker = {0};
    ev_bus_t bus;
    static uint32_t topic = 11;
    g_bus_post_cb = 0;
    expect_true(ev_loop_init(&owner) == 0 && ev_loop_init(&worker) == 0, "loops");
    expect_true(ev_bus_init(&bus, &owner) == 0, "bus");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_POST, &worker, bus_post_nop, NULL, ev_bus_match_u32, &topic) > 0, "sub");
    expect_true(ev_bus_publish(&bus, &topic, NULL) == 1, "queued");
    ev_loop_deinit(&worker);
    expect_true(g_bus_post_cb == 0, "cb not run");
    ev_bus_deinit(&bus);
    ev_loop_deinit(&owner);
    expect_true(1, "no crash");
}

static void test_bus_custom_match_null_arg(void)
{
    printf("[own/bus] 自定义 match 允许 match_arg=NULL\n");
    ev_loop_t loop = {0};
    ev_bus_t bus;
    g_sync_n = 0;
    expect_true(ev_loop_init(&loop) == 0 && ev_bus_init(&bus, &loop) == 0, "init");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_sync_count, NULL, match_always, NULL) > 0, "sub");
    uint32_t any = 0;
    expect_true(ev_bus_publish(&bus, &any, NULL) == 1, "hit");
    expect_true(g_sync_n == 1, "ok");
    ev_bus_deinit(&bus);
    ev_loop_deinit(&loop);
}

static void test_bus_match_u32_null_key(void)
{
    printf("[own/bus] match_u32：key/match_arg 为 NULL 不命中\n");
    expect_true(ev_bus_match_u32(NULL, NULL, NULL) == 0, "both null");
    uint32_t t = 1;
    expect_true(ev_bus_match_u32(NULL, &t, NULL) == 0, "key null");
    expect_true(ev_bus_match_u32(&t, NULL, NULL) == 0, "match_arg null");
    expect_true(ev_bus_match_u32(&t, &t, NULL) == 1, "equal");
}

/* ========================================================================== */

void test_ownership_and_boundaries(void)
{
    printf("======== ownership / boundary ========\n");
    test_fsm_free_arg_on_overflow();
    test_fsm_free_arg_unmatched();
    test_fsm_free_arg_after_action();
    test_fsm_deinit_during_poll_no_clear();
    test_fsm_fault_clears_queue_free_arg();
    test_fsm_null_free_arg_null_payload();

    test_loop_plain_post_deinit_does_not_free();
    test_loop_post_overflow_caller_frees();
    test_loop_nested_post_from_callback();
    test_loop_post_full_exactly_once();
    test_loop_post_full_fail_no_free_fn();
    test_loop_register_fsm_max();

    test_bus_publish_no_subscribers();
    test_bus_multi_sync_same_topic();
    test_bus_sub_max();
    test_bus_sync_stack_payload_ok();
    test_bus_post_target_deinit_frees_delivery();
    test_bus_custom_match_null_arg();
    test_bus_match_u32_null_key();
}
