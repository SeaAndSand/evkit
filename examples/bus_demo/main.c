/**
 * @file    main.c
 * @brief   ev_bus：match_u32（精确 topic）+ 自定义 range match（类 CAN ID 范围）
 *
 *   publish(TOPIC_TICK)     → SYNC 日志 + POST worker（ev_bus_match_u32）
 *   publish(can_id=0x120)   → SYNC 拦截原始「帧」（应用 range match）
 *
 * POST 的 tick 使用 static，保证活到 worker 回调；多 POST / 堆 payload 规则见 README。
 */
#include "evkit/ev_bus.h"
#include "evkit/ev_loop.h"

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static const uint32_t TOPIC_TICK = 1;

typedef struct {
    uint32_t lo;
    uint32_t hi;
} id_range_t;

typedef struct {
    uint32_t id;
    int data;
} fake_can_frame_t;

typedef struct {
    ev_loop_t *owner_loop;
    ev_bus_t *bus;
    int sync_hits;
    int post_hits;
    int range_hits;
} app_t;

/** 应用层：CAN ID 落在 [lo,hi] 则命中 */
static int match_id_range(const void *topic, const void *match_arg, void *user_data)
{
    (void)user_data;
    if (topic == NULL || match_arg == NULL) {
        return 0;
    }
    uint32_t id = *(const uint32_t *)topic;
    const id_range_t *r = match_arg;
    return (id >= r->lo && id <= r->hi) ? 1 : 0;
}

static void on_sync_log(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *t = topic;
    int *n = payload;
    app->sync_hits++;
    printf("[SYNC/u32] topic=%u n=%d (分发线程)\n", t != NULL ? *t : 0u, n != NULL ? *n : -1);
}

static void on_owner_done(void *user_data)
{
    app_t *app = user_data;
    printf("[owner] worker 已处理完，继续 CAN 范围演示后 stop\n");

    /* 演示自定义 match：发布 can_id，range 订阅者立刻收到原始帧 */
    static fake_can_frame_t frame = { .id = 0x120, .data = 7 };
    static uint32_t can_id = 0x120;
    can_id = frame.id;
    int n = ev_bus_publish(app->bus, &can_id, &frame);
    printf("[owner] can publish id=0x%x delivered=%d range_hits=%d\n", can_id, n, app->range_hits);
    ev_loop_stop(app->owner_loop);
}

static void on_post_worker(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *t = topic;
    int *n = payload;
    app->post_hits++;
    printf("[POST/u32] topic=%u n=%d (工作线程)\n", t != NULL ? *t : 0u, n != NULL ? *n : -1);
    (void)ev_loop_post(app->owner_loop, on_owner_done, app);
}

static void on_can_range(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *id = topic;
    fake_can_frame_t *fr = payload;
    app->range_hits++;
    printf("[SYNC/range] can_id=0x%x data=%d (分发线程，原始帧)\n", id != NULL ? *id : 0u, fr != NULL ? fr->data : -1);
}

static void *worker_main(void *arg)
{
    ev_loop_t *loop = arg;
    printf("[worker] loop running\n");
    (void)ev_loop_run(loop);
    printf("[worker] loop exit\n");
    return NULL;
}

static void on_start_publish(void *user_data)
{
    app_t *app = user_data;
    static int tick = 42;
    static uint32_t topic = 0;
    topic = TOPIC_TICK;
    int delivered = ev_bus_publish(app->bus, &topic, &tick);
    printf("[owner] tick publish delivered=%d\n", delivered);
}

int main(void)
{
    printf("=== bus_demo: match_u32 + custom range match ===\n");

    ev_loop_t owner = {0};
    ev_loop_t worker = {0};
    if (ev_loop_init(&owner) != 0 || ev_loop_init(&worker) != 0) {
        fprintf(stderr, "loop init failed\n");
        return 1;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, worker_main, &worker) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        ev_loop_deinit(&worker);
        ev_loop_deinit(&owner);
        return 1;
    }

    ev_bus_t bus;
    app_t app = {0};
    app.owner_loop = &owner;
    app.bus = &bus;

    /* match_arg 须活过订阅期：用静态/全局即可 */
    static uint32_t match_tick = 1; /* == TOPIC_TICK */
    static id_range_t match_can = { .lo = 0x100, .hi = 0x1ff };

    if (ev_bus_init(&bus, &owner) != 0) {
        fprintf(stderr, "bus init failed\n");
        return 1;
    }

    int id_sync = ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, on_sync_log, &app, ev_bus_match_u32, &match_tick);
    int id_post = ev_bus_subscribe(&bus, EV_BUS_POST, &worker, on_post_worker, &app, ev_bus_match_u32, &match_tick);
    int id_range = ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, on_can_range, &app, match_id_range, &match_can);
    if (id_sync < 0 || id_post < 0 || id_range < 0) {
        fprintf(stderr, "subscribe failed\n");
        return 1;
    }
    printf("[owner] sync=%d post=%d range=%d\n", id_sync, id_post, id_range);

    (void)ev_loop_post(&owner, on_start_publish, &app);
    (void)ev_loop_run(&owner);

    ev_loop_stop(&worker);
    (void)pthread_join(tid, NULL);

    printf("=== done sync=%d post=%d range=%d dropped=%u ===\n", app.sync_hits, app.post_hits, app.range_hits, bus.post_dropped);

    (void)ev_bus_unsubscribe(&bus, id_sync);
    (void)ev_bus_unsubscribe(&bus, id_post);
    (void)ev_bus_unsubscribe(&bus, id_range);
    ev_bus_deinit(&bus);
    ev_loop_deinit(&worker);
    ev_loop_deinit(&owner);

    return (app.sync_hits == 1 && app.post_hits == 1 && app.range_hits == 1) ? 0 : 1;
}
