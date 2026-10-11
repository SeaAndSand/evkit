/**
 * @file    test_evkit.c
 * @brief   evkit 回归：FSM 语义、队列溢出、timer 摘链、run 嵌套守卫、ev_bus
 */
#include "evkit/ev_bus.h"
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"
#include "test_ownership.h"
#include "test_pool.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 供 test_ownership.c 累计失败数 */
int g_test_failures;

static void expect_true(int cond, const char *name)
{
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", name);
        g_test_failures++;
    } else {
        printf("  ok %s\n", name);
    }
}

/* ---------- FSM ---------- */

enum { S0 = 0, S1, S2, S_COUNT };
enum { E_GO = 1, E_STAY, E_ANYSTOP, E_PING };

static int g_enter_s1;
static int g_exit_s0;
static int g_action_state_in_action = -1;

static void on_enter_s1(void *ud, int state)
{
    (void)ud;
    (void)state;
    g_enter_s1++;
}

static void on_exit_s0(void *ud, int state)
{
    (void)ud;
    (void)state;
    g_exit_s0++;
}

static void action_stay(void *ud, int event, void *arg)
{
    (void)event;
    (void)arg;
    ev_fsm_t *fsm = ud;
    g_action_state_in_action = ev_fsm_get_state(fsm);
}

static void action_ping_post(void *ud, int event, void *arg)
{
    (void)event;
    (void)arg;
    ev_fsm_t *fsm = ud;
    (void)ev_fsm_post(fsm, E_GO, NULL);
}

static const ev_fsm_transition_t k_edges[] = {
    { .from_state = S0, .event = E_GO, .to_state = S1, .action = NULL },
    { .from_state = S1, .event = E_STAY, .to_state = EV_FSM_STAY, .action = action_stay },
    { .from_state = S1, .event = E_PING, .to_state = EV_FSM_STAY, .action = action_ping_post },
    { .from_state = S0, .event = EV_FSM_ANY, .to_state = S2, .action = NULL },
    { .from_state = EV_FSM_ANY, .event = E_ANYSTOP, .to_state = S0, .action = NULL },
    { .from_state = S1, .event = E_GO, .to_state = S2, .action = NULL },
    { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = S0, .action = NULL },
};

static const ev_fsm_state_t k_states[S_COUNT] = {
    [S0] = { .name = "S0", .on_enter = NULL, .on_exit = on_exit_s0 },
    [S1] = { .name = "S1", .on_enter = on_enter_s1, .on_exit = NULL },
    [S2] = { .name = "S2", .on_enter = NULL, .on_exit = NULL },
};

static void test_fsm_exact_and_stay(void)
{
    printf("[fsm] exact / STAY / action 见旧状态\n");
    ev_fsm_t fsm;
    g_enter_s1 = 0;
    g_exit_s0 = 0;
    g_action_state_in_action = -1;
    ev_fsm_init(&fsm, S0, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, S_COUNT, &fsm, NULL);

    expect_true(ev_fsm_post(&fsm, E_GO, NULL) == 0, "post GO");
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == S1, "state S1");
    expect_true(g_exit_s0 == 1 && g_enter_s1 == 1, "exit/enter once");

    expect_true(ev_fsm_post(&fsm, E_STAY, NULL) == 0, "post STAY");
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == S1, "still S1");
    expect_true(g_enter_s1 == 1, "STAY 不触发 enter");
    expect_true(g_action_state_in_action == S1, "action 内仍是旧状态");
}

static void test_fsm_wildcard_priority(void)
{
    printf("[fsm] 通配优先级\n");
    ev_fsm_t fsm;
    ev_fsm_init(&fsm, S1, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, S_COUNT, &fsm, NULL);

    (void)ev_fsm_post(&fsm, E_GO, NULL);
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == S2, "exact beats wildcard");

    ev_fsm_init(&fsm, S0, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, S_COUNT, &fsm, NULL);
    (void)ev_fsm_post(&fsm, 999, NULL);
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == S2, "state-exact+event-ANY wins");
}

static void test_fsm_overflow_and_repost(void)
{
    printf("[fsm] 溢出与 action 内 post\n");
    ev_fsm_t fsm;
    ev_fsm_init(&fsm, S1, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, S_COUNT, &fsm, NULL);

    int filled = 1;
    for (int i = 0; i < EV_FSM_QUEUE_CAPACITY; i++) {
        if (ev_fsm_post(&fsm, E_STAY, NULL) != 0) {
            filled = 0;
            break;
        }
    }
    expect_true(filled, "fill queue");
    expect_true(ev_fsm_post(&fsm, E_STAY, NULL) == -1, "overflow -1");
    expect_true(fsm.post_dropped == 1, "fsm post_dropped");

    ev_fsm_poll(&fsm);

    (void)ev_fsm_post(&fsm, E_PING, NULL);
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == S2, "action post GO → S2");
}

static void on_enter_loop(void *ud, int state)
{
    (void)state;
    ev_fsm_t *fsm = ud;
    (void)ev_fsm_post(fsm, E_GO, NULL);
}

static const ev_fsm_transition_t k_loop_edges[] = {
    { .from_state = S0, .event = E_GO, .to_state = S1, .action = NULL },
    { .from_state = S1, .event = E_GO, .to_state = S0, .action = NULL },
    { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = S2, .action = NULL },
};

static const ev_fsm_state_t k_loop_states[S_COUNT] = {
    [S0] = { .name = "S0", .on_enter = on_enter_loop, .on_exit = NULL },
    [S1] = { .name = "S1", .on_enter = on_enter_loop, .on_exit = NULL },
    [S2] = { .name = "S2", .on_enter = NULL, .on_exit = NULL },
};

static void test_fsm_poll_limit(void)
{
    printf("[fsm] poll 步数上限 → FAULT\n");
    ev_fsm_t fsm = {0};
    ev_fsm_init(&fsm, S0, k_loop_edges, sizeof(k_loop_edges) / sizeof(k_loop_edges[0]), k_loop_states, S_COUNT, &fsm, NULL);
    ev_fsm_poll(&fsm);
    expect_true(fsm.poll_limit_hits >= 1, "poll_limit_hits");
    expect_true(ev_fsm_queue_len(&fsm) == 0, "queue cleared after FAULT");
    expect_true(ev_fsm_get_state(&fsm) == S2, "FAULT → S2");
    ev_fsm_deinit(&fsm);
}

static int g_freed_args;

static void free_arg_count(void *arg)
{
    if (arg == NULL) {
        return;
    }
    g_freed_args++;
    free(arg);
}

static void test_fsm_deinit_frees_pending(void)
{
    printf("[fsm] deinit 清队走 free_arg\n");
    static const ev_fsm_transition_t edges[] = {
        { .from_state = S0, .event = E_GO, .to_state = S1, .action = NULL },
        { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = S0, .action = NULL },
    };
    ev_fsm_t fsm = {0};
    g_freed_args = 0;
    expect_true(ev_fsm_init(&fsm, S0, edges, sizeof(edges) / sizeof(edges[0]), NULL, 0, NULL, free_arg_count) == 0, "init");
    void *p = malloc(1);
    expect_true(p != NULL, "malloc");
    expect_true(ev_fsm_post(&fsm, E_GO, p) == 0, "post heap arg");
    expect_true(ev_fsm_queue_len(&fsm) == 1, "queued");
    ev_fsm_deinit(&fsm);
    expect_true(g_freed_args == 1, "free_arg called");
    expect_true(ev_fsm_queue_len(&fsm) == 0, "queue empty");
}

/* ---------- loop ---------- */

static int g_nested_rc;

static void post_try_nested_run(void *ud)
{
    ev_loop_t *loop = ud;
    g_nested_rc = ev_loop_run(loop);
    ev_loop_stop(loop);
}

static void post_noop(void *ud)
{
    (void)ud;
}

static void post_stop(void *ud)
{
    ev_loop_stop(ud);
}

static void test_loop_nested_guard(void)
{
    printf("[loop] 嵌套 run 守卫\n");
    ev_loop_t loop;
    expect_true(ev_loop_init(&loop) == 0, "init");
    g_nested_rc = 0;
    expect_true(ev_loop_post(&loop, post_try_nested_run, &loop) == 0, "post nested");
    expect_true(ev_loop_run(&loop) == 0, "outer run ok");
    expect_true(g_nested_rc == -1, "nested run -1");
    ev_loop_deinit(&loop);
}

static int g_timer_fired;

static void on_one_shot(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)timer;
    g_timer_fired++;
    ev_loop_stop(loop);
}

static void test_loop_timer_remove(void)
{
    printf("[loop] timer remove 摘链 + after/repeat\n");
    ev_loop_t loop;
    expect_true(ev_loop_init(&loop) == 0, "init");

    ev_timer_t t1;
    ev_timer_t t2;
    memset(&t1, 0, sizeof(t1));
    memset(&t2, 0, sizeof(t2));

    expect_true(ev_timer_start(&loop, &t1, 0, 0, on_one_shot, NULL) == 0, "start after=0 one-shot");
    expect_true(t1.interval_ms == 0, "repeat=0 → one-shot");
    expect_true(ev_timer_start(&loop, &t2, 10, 0, on_one_shot, NULL) == 0, "start t2");
    expect_true(ev_timer_remove(&t2) == 0, "remove t2");
    expect_true(t2.loop == NULL && t2.next == NULL, "t2 unlinked");

    g_timer_fired = 0;
    expect_true(ev_loop_run(&loop) == 0, "run until t1");
    expect_true(g_timer_fired == 1, "t1 fired once");

    ev_timer_remove(&t1);
    ev_loop_deinit(&loop);
}

static void test_loop_post_overflow(void)
{
    printf("[loop] post 队列溢出\n");
    ev_loop_t loop;
    expect_true(ev_loop_init(&loop) == 0, "init");

    int filled = 1;
    for (uint32_t i = 0; i < EV_LOOP_POST_CAPACITY; i++) {
        if (ev_loop_post(&loop, post_noop, NULL) != 0) {
            filled = 0;
            break;
        }
    }
    expect_true(filled, "fill post");
    expect_true(ev_loop_post(&loop, post_noop, NULL) == -1, "post full");
    expect_true(loop.post_queue_dropped == 1, "post_queue_dropped");
    ev_loop_deinit(&loop);
}

static void test_loop_io_remove(void)
{
    printf("[loop] ev_io_remove 清 callback\n");
    ev_loop_t loop;
    expect_true(ev_loop_init(&loop) == 0, "init");

    ev_io_t mark = { .fd = -1, .events = 0, .callback = (ev_io_fn)(uintptr_t)1, .user_data = NULL };
    expect_true(ev_io_remove(&loop, &mark) == 0, "remove fd=-1");
    expect_true(mark.callback == NULL, "callback cleared");

    expect_true(ev_loop_post(&loop, post_stop, &loop) == 0, "post stop");
    expect_true(ev_loop_run(&loop) == 0, "run stop");
    ev_loop_deinit(&loop);
}

static void test_loop_stop_before_run(void)
{
    printf("[loop] run 前 stop 立即返回\n");
    ev_loop_t loop;
    expect_true(ev_loop_init(&loop) == 0, "init");
    ev_loop_stop(&loop);
    expect_true(ev_loop_run(&loop) == 0, "run honors pre-stop");
    expect_true(atomic_load(&loop.stop_requested) == 0, "stop cleared on exit");

    /* 再次 run 应可正常跑，再由 post stop 退出 */
    expect_true(ev_loop_post(&loop, post_stop, &loop) == 0, "post stop for 2nd run");
    expect_true(ev_loop_run(&loop) == 0, "2nd run ok");
    ev_loop_deinit(&loop);
}

/* 同批到期：回调里 destroy+free 兄弟 timer，不得 UAF */
static ev_timer_t *g_heap_a;
static ev_timer_t *g_heap_b;
static int g_heap_a_fired;

static void on_heap_a(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)timer;
    g_heap_a_fired++;
    ev_timer_remove(g_heap_b);
    free(g_heap_b);
    g_heap_b = NULL;
    ev_loop_stop(loop);
}

static void on_heap_b(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    (void)timer;
    /* 若 A 先触发并 free 了 B，B 不应再被回调；若被调用也只是计数 */
}

static void test_loop_timer_free_sibling(void)
{
    printf("[loop] timer 回调 remove+free 兄弟\n");
    ev_loop_t loop;
    expect_true(ev_loop_init(&loop) == 0, "init");

    g_heap_a = calloc(1, sizeof(*g_heap_a));
    g_heap_b = calloc(1, sizeof(*g_heap_b));
    expect_true(g_heap_a != NULL && g_heap_b != NULL, "alloc timers");

    /* 先挂 B 再挂 A → 链表头为 A→B；A 回调里 free B */
    expect_true(ev_timer_start(&loop, g_heap_b, 10, 0, on_heap_b, NULL) == 0, "start B");
    expect_true(ev_timer_start(&loop, g_heap_a, 10, 0, on_heap_a, NULL) == 0, "start A");

    g_heap_a_fired = 0;
    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(g_heap_a_fired == 1, "A fired");
    expect_true(g_heap_b == NULL, "B freed");

    ev_timer_remove(g_heap_a);
    free(g_heap_a);
    g_heap_a = NULL;
    ev_loop_deinit(&loop);
}

/* ---------- 跨线程 post（类 GMainLoop invoke） ---------- */

typedef struct {
    ev_loop_t *loop;
    int hits;
    pthread_t tid;
} cross_post_ctx_t;

static void post_cross_hit(void *ud)
{
    cross_post_ctx_t *ctx = ud;
    ctx->hits++;
    if (ctx->hits >= 8) {
        ev_loop_stop(ctx->loop);
    }
}

static void *cross_poster_thread(void *arg)
{
    cross_post_ctx_t *ctx = arg;
    for (int i = 0; i < 8; i++) {
        while (ev_loop_post(ctx->loop, post_cross_hit, ctx) != 0) {
            /* 队列偶发满则重试 */
        }
    }
    return NULL;
}

/* post_full：deinit 丢弃时调用 free_fn，不跑 callback */
static int g_post_full_cb;
static int g_post_full_free;

static void post_full_cb(void *ud)
{
    (void)ud;
    g_post_full_cb++;
}

static void post_full_free(void *ud)
{
    free(ud);
    g_post_full_free++;
}

static void test_loop_post_full_deinit_frees(void)
{
    printf("[loop] post_full：deinit 丢弃走 free_fn\n");
    ev_loop_t loop = {0};
    expect_true(ev_loop_init(&loop) == 0, "init");
    g_post_full_cb = 0;
    g_post_full_free = 0;
    void *p = malloc(8);
    expect_true(p != NULL, "malloc");
    expect_true(ev_loop_post_full(&loop, post_full_cb, p, post_full_free) == 0, "post_full");
    ev_loop_deinit(&loop); /* 不 run：应 free，不 cb */
    expect_true(g_post_full_cb == 0, "no callback");
    expect_true(g_post_full_free == 1, "free once");
}

static void test_loop_cross_thread_post(void)
{
    printf("[loop] 跨线程 ev_loop_post / stop\n");
    ev_loop_t loop;
    cross_post_ctx_t ctx;

    expect_true(ev_loop_init(&loop) == 0, "init");
    memset(&ctx, 0, sizeof(ctx));
    ctx.loop = &loop;

    expect_true(pthread_create(&ctx.tid, NULL, cross_poster_thread, &ctx) == 0, "spawn poster");
    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(pthread_join(ctx.tid, NULL) == 0, "join");
    expect_true(ctx.hits == 8, "8 post callbacks on loop thread");
    ev_loop_deinit(&loop);
}

static uint64_t g_align_first_deadline;
static int g_align_fires;
static int g_align_ok;

static void on_align_tick(ev_loop_t *loop, ev_timer_t *timer)
{
    g_align_fires++;
    /* 回调前已排好下次：应为 first + n*interval */
    uint64_t expect = g_align_first_deadline + (uint64_t)g_align_fires * timer->interval_ms;
    if (timer->deadline_ms != expect) {
        g_align_ok = 0;
    }
    if (g_align_fires >= 2) {
        ev_loop_stop(loop);
    }
}

static void test_loop_timer_start_at_align(void)
{
    printf("[loop] start_at 周期按 deadline 对齐\n");
    ev_loop_t loop;
    expect_true(ev_loop_init(&loop) == 0, "init");

    ev_timer_t t;
    memset(&t, 0, sizeof(t));
    g_align_first_deadline = ev_loop_now_ms_u64() + 20;
    g_align_fires = 0;
    g_align_ok = 1;
    expect_true(ev_timer_start_at(&loop, &t, g_align_first_deadline, 30, on_align_tick, NULL) == 0, "start_at");
    expect_true(t.repeat_from_deadline == 1, "repeat_from_deadline");

    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(g_align_fires == 2, "fired twice");
    expect_true(g_align_ok, "deadlines aligned");

    ev_timer_remove(&t);
    ev_loop_deinit(&loop);
}

/* ---------- ev_bus：SYNC + POST + match ---------- */

static const uint32_t BUS_TOPIC = 7;

typedef struct {
    uint32_t lo;
    uint32_t hi;
} bus_range_t;

typedef struct {
    int sync_hits;
    int post_hits;
    int range_hits;
    int last_value;
    ev_loop_t *owner;
    ev_loop_t *worker;
} bus_test_ctx_t;

static int bus_match_range(const void *topic, const void *match_arg, void *user_data)
{
    (void)user_data;
    if (topic == NULL || match_arg == NULL) {
        return 0;
    }
    uint32_t id = *(const uint32_t *)topic;
    const bus_range_t *r = match_arg;
    return (id >= r->lo && id <= r->hi) ? 1 : 0;
}

static void bus_on_sync(const void *topic, void *payload, void *user_data)
{
    bus_test_ctx_t *ctx = user_data;
    (void)topic;
    ctx->sync_hits++;
    if (payload != NULL) {
        ctx->last_value = *(int *)payload;
    }
}

static void bus_on_post(const void *topic, void *payload, void *user_data)
{
    bus_test_ctx_t *ctx = user_data;
    (void)topic;
    ctx->post_hits++;
    if (payload != NULL) {
        ctx->last_value = *(int *)payload;
    }
    ev_loop_stop(ctx->worker);
    ev_loop_stop(ctx->owner);
}

static void bus_on_range(const void *topic, void *payload, void *user_data)
{
    bus_test_ctx_t *ctx = user_data;
    (void)topic;
    (void)payload;
    ctx->range_hits++;
}

static void *bus_worker_thread(void *arg)
{
    ev_loop_t *loop = arg;
    (void)ev_loop_run(loop);
    return NULL;
}

typedef struct {
    ev_bus_t *bus;
} bus_pub_arg_t;

static void bus_do_publish(void *user_data)
{
    bus_pub_arg_t *a = user_data;
    static int value = 99;
    static uint32_t topic = 0;
    topic = BUS_TOPIC;
    (void)ev_bus_publish(a->bus, &topic, &value);
}

static void test_bus_sync_and_post(void)
{
    printf("[bus] SYNC/POST + match_u32 + range match\n");
    ev_loop_t owner = {0};
    ev_loop_t worker = {0};
    ev_bus_t bus;
    bus_test_ctx_t ctx = {0};
    bus_pub_arg_t parg;
    static uint32_t match_topic = 7;
    static bus_range_t match_range = { .lo = 0x10, .hi = 0x20 };

    expect_true(ev_loop_init(&owner) == 0, "owner init");
    expect_true(ev_loop_init(&worker) == 0, "worker init");
    expect_true(ev_bus_init(&bus, &owner) == 0, "bus init");

    ctx.owner = &owner;
    ctx.worker = &worker;
    parg.bus = &bus;

    int id_s = ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_on_sync, &ctx, ev_bus_match_u32, &match_topic);
    int id_p = ev_bus_subscribe(&bus, EV_BUS_POST, &worker, bus_on_post, &ctx, ev_bus_match_u32, &match_topic);
    int id_r = ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_on_range, &ctx, bus_match_range, &match_range);
    expect_true(id_s > 0 && id_p > 0 && id_r > 0, "subscribe");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_POST, NULL, bus_on_post, &ctx, ev_bus_match_u32, &match_topic) == -1,
                "POST needs loop");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, bus_on_sync, &ctx, NULL, &match_topic) == -1, "match required");

    /* 自定义 range：只打中 range 订阅，不打中 u32 topic=7 */
    uint32_t in_range = 0x15;
    expect_true(ev_bus_publish(&bus, &in_range, NULL) == 1, "range hit only");
    expect_true(ctx.range_hits == 1 && ctx.sync_hits == 0 && ctx.post_hits == 0, "only range cb");

    pthread_t tid;
    expect_true(pthread_create(&tid, NULL, bus_worker_thread, &worker) == 0, "spawn worker");
    expect_true(ev_loop_post(&owner, bus_do_publish, &parg) == 0, "schedule publish");
    expect_true(ev_loop_run(&owner) == 0, "owner run");
    expect_true(pthread_join(tid, NULL) == 0, "join worker");

    expect_true(ctx.sync_hits == 1, "sync once");
    expect_true(ctx.post_hits == 1, "post once");
    expect_true(ctx.last_value == 99, "payload value");
    expect_true(ev_bus_unsubscribe(&bus, id_s) == 0, "unsub sync");
    expect_true(ev_bus_unsubscribe(&bus, id_p) == 0, "unsub post");
    expect_true(ev_bus_unsubscribe(&bus, id_r) == 0, "unsub range");
    expect_true(ev_bus_publish(&bus, &match_topic, NULL) == 0, "publish after unsub");

    ev_bus_deinit(&bus);
    ev_loop_deinit(&worker);
    ev_loop_deinit(&owner);
}

/* 多 POST 共享同一 payload：须活到全部回调结束（用 static） */
typedef struct {
    int hits;
    int sum;
    ev_loop_t *loops[2];
    int stop_when;
} multi_post_ctx_t;

static void bus_on_multi_post(const void *topic, void *payload, void *user_data)
{
    multi_post_ctx_t *ctx = user_data;
    (void)topic;
    ctx->hits++;
    if (payload != NULL) {
        ctx->sum += *(int *)payload;
    }
    if (ctx->hits >= ctx->stop_when) {
        ev_loop_stop(ctx->loops[0]);
        ev_loop_stop(ctx->loops[1]);
    }
}

static void *bus_run_loop(void *arg)
{
    (void)ev_loop_run((ev_loop_t *)arg);
    return NULL;
}

static void test_bus_multi_post_shared_payload(void)
{
    printf("[bus] 多 POST 共享 static payload\n");
    ev_loop_t owner = {0};
    ev_loop_t w0 = {0};
    ev_loop_t w1 = {0};
    ev_bus_t bus;
    multi_post_ctx_t ctx = {0};
    static uint32_t topic = 42;
    static int payload = 10;

    expect_true(ev_loop_init(&owner) == 0 && ev_loop_init(&w0) == 0 && ev_loop_init(&w1) == 0, "loops");
    expect_true(ev_bus_init(&bus, &owner) == 0, "bus");
    ctx.loops[0] = &w0;
    ctx.loops[1] = &w1;
    ctx.stop_when = 2;

    expect_true(ev_bus_subscribe(&bus, EV_BUS_POST, &w0, bus_on_multi_post, &ctx, ev_bus_match_u32, &topic) > 0, "sub0");
    expect_true(ev_bus_subscribe(&bus, EV_BUS_POST, &w1, bus_on_multi_post, &ctx, ev_bus_match_u32, &topic) > 0, "sub1");

    pthread_t t0, t1;
    expect_true(pthread_create(&t0, NULL, bus_run_loop, &w0) == 0, "t0");
    expect_true(pthread_create(&t1, NULL, bus_run_loop, &w1) == 0, "t1");
    expect_true(ev_bus_publish(&bus, &topic, &payload) == 2, "publish 2");
    expect_true(pthread_join(t0, NULL) == 0 && pthread_join(t1, NULL) == 0, "join");
    expect_true(ctx.hits == 2 && ctx.sum == 20, "both got payload");

    ev_bus_deinit(&bus);
    ev_loop_deinit(&w0);
    ev_loop_deinit(&w1);
    ev_loop_deinit(&owner);
}

/* 退订不撤销已入队 POST */
typedef struct {
    int hits;
    ev_loop_t *worker;
    ev_loop_t *owner;
} inflight_ctx_t;

static void bus_on_inflight(const void *topic, void *payload, void *user_data)
{
    inflight_ctx_t *ctx = user_data;
    (void)topic;
    (void)payload;
    ctx->hits++;
    ev_loop_stop(ctx->worker);
    ev_loop_stop(ctx->owner);
}

static void test_bus_unsub_inflight_still_runs(void)
{
    printf("[bus] unsubscribe 后已入队 POST 仍回调\n");
    ev_loop_t owner = {0};
    ev_loop_t worker = {0};
    ev_bus_t bus;
    inflight_ctx_t ctx = {0};
    static uint32_t topic = 99;

    expect_true(ev_loop_init(&owner) == 0 && ev_loop_init(&worker) == 0, "loops");
    expect_true(ev_bus_init(&bus, &owner) == 0, "bus");
    ctx.worker = &worker;
    ctx.owner = &owner;

    int id = ev_bus_subscribe(&bus, EV_BUS_POST, &worker, bus_on_inflight, &ctx, ev_bus_match_u32, &topic);
    expect_true(id > 0, "subscribe");
    expect_true(ev_bus_publish(&bus, &topic, NULL) == 1, "publish queues");
    expect_true(ev_bus_unsubscribe(&bus, id) == 0, "unsub");

    pthread_t tid;
    expect_true(pthread_create(&tid, NULL, bus_run_loop, &worker) == 0, "spawn");
    /* owner 也需 run 才能被 stop；用 post 停 owner 若 worker 先结束 */
    expect_true(ev_loop_run(&owner) == 0, "owner run");
    expect_true(pthread_join(tid, NULL) == 0, "join");
    expect_true(ctx.hits == 1, "inflight cb ran");

    ev_bus_deinit(&bus);
    ev_loop_deinit(&worker);
    ev_loop_deinit(&owner);
}

/* post_full 正常执行：callback 后 free 一次 */
static void test_loop_post_full_run_frees(void)
{
    printf("[loop] post_full：run 后 callback+free 各一次\n");
    ev_loop_t loop = {0};
    g_post_full_cb = 0;
    g_post_full_free = 0;
    expect_true(ev_loop_init(&loop) == 0, "init");
    void *p = malloc(4);
    expect_true(p != NULL, "malloc");
    expect_true(ev_loop_post_full(&loop, post_full_cb, p, post_full_free) == 0, "post");
    expect_true(ev_loop_post(&loop, post_stop, &loop) == 0, "stop");
    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(g_post_full_cb == 1, "cb once");
    expect_true(g_post_full_free == 1, "free once");
    ev_loop_deinit(&loop);
}

/* unregister 自身不得使本轮后续 FSM 漏 poll */
enum { U_ST = 0 };
enum { U_EV = 1 };

static ev_loop_t *g_unreg_loop;
static ev_fsm_t *g_unreg_a;
static int g_unreg_hit_a;
static int g_unreg_hit_b;
static int g_unreg_hit_c;

static void unreg_action_a(void *ud, int event, void *arg)
{
    (void)ud;
    (void)event;
    (void)arg;
    g_unreg_hit_a++;
    (void)ev_loop_unregister_fsm(g_unreg_loop, g_unreg_a);
}

static void unreg_action_b(void *ud, int event, void *arg)
{
    (void)ud;
    (void)event;
    (void)arg;
    g_unreg_hit_b++;
}

static void unreg_action_c(void *ud, int event, void *arg)
{
    (void)ud;
    (void)event;
    (void)arg;
    g_unreg_hit_c++;
}

static void test_loop_unregister_during_poll_no_skip(void)
{
    printf("[loop] poll 中 unregister 自身不跳过后续 FSM\n");
    ev_loop_t loop = {0};
    ev_fsm_t a = {0};
    ev_fsm_t b = {0};
    ev_fsm_t c = {0};
    static const ev_fsm_transition_t edges_a[] = {
        { .from_state = U_ST, .event = U_EV, .to_state = EV_FSM_STAY, .action = unreg_action_a },
        { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = U_ST, .action = NULL },
    };
    static const ev_fsm_transition_t edges_b[] = {
        { .from_state = U_ST, .event = U_EV, .to_state = EV_FSM_STAY, .action = unreg_action_b },
        { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = U_ST, .action = NULL },
    };
    static const ev_fsm_transition_t edges_c[] = {
        { .from_state = U_ST, .event = U_EV, .to_state = EV_FSM_STAY, .action = unreg_action_c },
        { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = U_ST, .action = NULL },
    };

    g_unreg_loop = &loop;
    g_unreg_a = &a;
    g_unreg_hit_a = g_unreg_hit_b = g_unreg_hit_c = 0;

    expect_true(ev_loop_init(&loop) == 0, "init");
    expect_true(ev_fsm_init(&a, U_ST, edges_a, 2, NULL, 0, NULL, NULL) == 0, "init a");
    expect_true(ev_fsm_init(&b, U_ST, edges_b, 2, NULL, 0, NULL, NULL) == 0, "init b");
    expect_true(ev_fsm_init(&c, U_ST, edges_c, 2, NULL, 0, NULL, NULL) == 0, "init c");
    expect_true(ev_loop_register_fsm(&loop, &a) == 0, "reg a");
    expect_true(ev_loop_register_fsm(&loop, &b) == 0, "reg b");
    expect_true(ev_loop_register_fsm(&loop, &c) == 0, "reg c");
    expect_true(ev_fsm_post(&a, U_EV, NULL) == 0, "post a");
    expect_true(ev_fsm_post(&b, U_EV, NULL) == 0, "post b");
    expect_true(ev_fsm_post(&c, U_EV, NULL) == 0, "post c");
    expect_true(ev_loop_post(&loop, post_stop, &loop) == 0, "stop");
    expect_true(ev_loop_run(&loop) == 0, "run");
    expect_true(g_unreg_hit_a == 1, "A polled");
    expect_true(g_unreg_hit_b == 1, "B polled");
    expect_true(g_unreg_hit_c == 1, "C polled");

    (void)ev_loop_unregister_fsm(&loop, &b);
    (void)ev_loop_unregister_fsm(&loop, &c);
    ev_fsm_deinit(&a);
    ev_fsm_deinit(&b);
    ev_fsm_deinit(&c);
    ev_loop_deinit(&loop);
}

static void test_fsm_fault_requires_any(void)
{
    printf("[fsm] FAULT 边须 from_state==ANY\n");
    ev_fsm_t fsm = {0};
    static const ev_fsm_transition_t bad[] = {
        { .from_state = S0, .event = EV_FSM_FAULT, .to_state = S0, .action = NULL },
    };
    expect_true(ev_fsm_init(&fsm, S0, bad, 1, NULL, 0, NULL, NULL) == -1, "reject non-ANY FAULT");
}

static int g_fault_recover_posts;

static void action_fault_post_recover(void *ud, int event, void *arg)
{
    (void)event;
    (void)arg;
    ev_fsm_t *fsm = ud;
    g_fault_recover_posts++;
    (void)ev_fsm_post(fsm, E_GO, NULL);
}

static void test_fsm_fault_keeps_posts_for_next_poll(void)
{
    printf("[fsm] FAULT action post 保留到下一次 poll\n");
    ev_fsm_t fsm = {0};
    static const ev_fsm_transition_t edges[] = {
        { .from_state = S0, .event = E_GO, .to_state = S1, .action = NULL },
        { .from_state = S2, .event = E_GO, .to_state = S1, .action = NULL },
        { .from_state = EV_FSM_ANY, .event = EV_FSM_FAULT, .to_state = S2, .action = action_fault_post_recover },
    };
    g_fault_recover_posts = 0;
    expect_true(ev_fsm_init(&fsm, S0, edges, 3, NULL, 0, &fsm, NULL) == 0, "init");
    expect_true(ev_fsm_post(&fsm, EV_FSM_FAULT, NULL) == 0, "post FAULT");
    expect_true(ev_fsm_post(&fsm, E_GO, NULL) == 0, "post backlog");
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == S2, "FAULT → S2");
    expect_true(g_fault_recover_posts == 1, "fault action ran");
    expect_true(ev_fsm_queue_len(&fsm) == 1, "recover kept");
    ev_fsm_poll(&fsm);
    expect_true(ev_fsm_get_state(&fsm) == S1, "recover → S1");
    expect_true(ev_fsm_queue_len(&fsm) == 0, "empty");
    ev_fsm_deinit(&fsm);
}

int main(void)
{
    printf("======== evkit unit tests ========\n");
    test_fsm_exact_and_stay();
    test_fsm_wildcard_priority();
    test_fsm_overflow_and_repost();
    test_fsm_poll_limit();
    test_fsm_fault_requires_any();
    test_fsm_fault_keeps_posts_for_next_poll();
    test_fsm_deinit_frees_pending();
    test_loop_nested_guard();
    test_loop_timer_remove();
    test_loop_timer_start_at_align();
    test_loop_post_overflow();
    test_loop_io_remove();
    test_loop_stop_before_run();
    test_loop_timer_free_sibling();
    test_loop_cross_thread_post();
    test_loop_post_full_deinit_frees();
    test_loop_post_full_run_frees();
    test_loop_unregister_during_poll_no_skip();
    test_bus_sync_and_post();
    test_bus_multi_post_shared_payload();
    test_bus_unsub_inflight_still_runs();
    test_ownership_and_boundaries();
    test_pool_suite();

    if (g_test_failures != 0) {
        fprintf(stderr, "======== %d failure(s) ========\n", g_test_failures);
        return 1;
    }
    printf("======== all passed ========\n");
    return 0;
}
