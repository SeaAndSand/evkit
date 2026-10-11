/**
 * @file    main.c
 * @brief   总线扇出真用法：采集线程 → post 到 owner → publish；SYNC + POST + 环缓保活
 *
 * 流程:
 *   sensor 线程定时采样，写入环缓后把「槽位 id」post 到 owner
 *   owner 回调里 ev_bus_publish(topic, &ring->slots[slot])
 *     - SYNC：分发线程立刻读样本
 *     - POST：工作线程稍后读同一环缓槽（payload 指向环内对象，勿指向栈临时量）
 *
 * SAMPLE_COUNT < RING_CAP，保证本 demo 结束前槽不被覆盖。
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_bus.h"
#include "evkit/ev_loop.h"

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define RING_CAP 8
#define SAMPLE_COUNT 5

enum {
    TOPIC_SAMPLE = 1,
    TOPIC_ALARM = 2
};

typedef struct {
    uint32_t seq;
    int value;
    int alarm;
} sample_t;

typedef struct {
    sample_t slots[RING_CAP];
    uint32_t write_seq;
} ring_t;

typedef struct {
    ev_loop_t *owner;
    ev_loop_t *worker;
    ev_bus_t *bus;
    ring_t *ring;
    int sync_hits;
    int post_hits;
    int alarm_hits;
    int samples_done;
    int stop_posted;
} app_t;

typedef struct {
    app_t *app;
    uint32_t slot;
} ingest_t;

static const uint32_t k_match_sample = TOPIC_SAMPLE;
static const uint32_t k_match_alarm = TOPIC_ALARM;

static void ring_put(ring_t *r, const sample_t *s, uint32_t *out_slot)
{
    uint32_t slot = r->write_seq % RING_CAP;
    r->slots[slot] = *s;
    r->write_seq++;
    *out_slot = slot;
}

static void owner_maybe_stop(app_t *app)
{
    if (app->stop_posted) {
        return;
    }
    if (app->samples_done < SAMPLE_COUNT || app->post_hits < SAMPLE_COUNT) {
        return;
    }
    app->stop_posted = 1;
    printf("[owner] samples+post done, stop\n");
    ev_loop_stop(app->owner);
}

static void owner_on_check_stop(void *user_data)
{
    owner_maybe_stop(user_data);
}

/** owner：收到槽位后 publish；payload 指向环内样本（POST 保活） */
static void owner_on_ingest(void *user_data)
{
    ingest_t *in = user_data;
    app_t *app = in->app;
    uint32_t slot = in->slot;
    free(in);

    if (slot >= RING_CAP) {
        return;
    }
    sample_t *s = &app->ring->slots[slot];

    static uint32_t pub_sample = TOPIC_SAMPLE;
    int n = ev_bus_publish(app->bus, &pub_sample, s);
    app->samples_done++;
    printf("[owner] publish SAMPLE seq=%u value=%d slot=%u delivered=%d\n", s->seq, s->value, slot, n);

    if (s->alarm) {
        static uint32_t pub_alarm = TOPIC_ALARM;
        int na = ev_bus_publish(app->bus, &pub_alarm, s);
        printf("[owner] publish ALARM seq=%u delivered=%d\n", s->seq, na);
    }
    owner_maybe_stop(app);
}

static void on_sync_sample(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *t = topic;
    const sample_t *s = payload;
    app->sync_hits++;
    printf("[SYNC] topic=%u seq=%u value=%d (owner thread)\n",
           t != NULL ? *t : 0u,
           s != NULL ? s->seq : 0u,
           s != NULL ? s->value : -1);
}

static void on_post_sample(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *t = topic;
    const sample_t *s = payload;
    app->post_hits++;
    printf("[POST] topic=%u seq=%u value=%d (worker thread)\n",
           t != NULL ? *t : 0u,
           s != NULL ? s->seq : 0u,
           s != NULL ? s->value : -1);
    (void)ev_loop_post(app->owner, owner_on_check_stop, app);
}

static void on_sync_alarm(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    (void)topic;
    const sample_t *s = payload;
    app->alarm_hits++;
    printf("[SYNC/ALARM] seq=%u value=%d\n", s != NULL ? s->seq : 0u, s != NULL ? s->value : -1);
}

static void *sensor_thread(void *arg)
{
    app_t *app = arg;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        usleep(40 * 1000);
        sample_t s = {
            .seq = (uint32_t)(i + 1),
            .value = 100 + i,
            .alarm = (i == 2) ? 1 : 0,
        };
        uint32_t slot = 0;
        ring_put(app->ring, &s, &slot);

        ingest_t *in = calloc(1, sizeof(*in));
        if (in == NULL) {
            continue;
        }
        in->app = app;
        in->slot = slot;
        printf("[sensor] sample seq=%u → owner slot=%u\n", s.seq, slot);
        if (ev_loop_post(app->owner, owner_on_ingest, in) != 0) {
            fprintf(stderr, "[sensor] post full\n");
            free(in);
        }
    }
    return NULL;
}

static void *worker_thread(void *arg)
{
    ev_loop_t *loop = arg;
    printf("[worker] loop running\n");
    (void)ev_loop_run(loop);
    printf("[worker] loop exit\n");
    return NULL;
}

int main(void)
{
    printf("=== topic_hub: sensor → bus SYNC/POST + ring slot ===\n");

    ev_loop_t owner = {0};
    ev_loop_t worker = {0};
    ring_t ring = {0};
    if (ev_loop_init(&owner) != 0 || ev_loop_init(&worker) != 0) {
        fprintf(stderr, "loop init failed\n");
        return 1;
    }

    ev_bus_t bus;
    app_t app = {0};
    app.owner = &owner;
    app.worker = &worker;
    app.bus = &bus;
    app.ring = &ring;

    if (ev_bus_init(&bus, &owner) != 0) {
        fprintf(stderr, "bus init failed\n");
        return 1;
    }

    int id_sync = ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, on_sync_sample, &app, ev_bus_match_u32, &k_match_sample);
    int id_post = ev_bus_subscribe(&bus, EV_BUS_POST, &worker, on_post_sample, &app, ev_bus_match_u32, &k_match_sample);
    int id_alarm = ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, on_sync_alarm, &app, ev_bus_match_u32, &k_match_alarm);
    if (id_sync < 0 || id_post < 0 || id_alarm < 0) {
        fprintf(stderr, "subscribe failed\n");
        return 1;
    }

    pthread_t wid;
    pthread_t sid;
    if (pthread_create(&wid, NULL, worker_thread, &worker) != 0) {
        fprintf(stderr, "worker pthread failed\n");
        return 1;
    }
    if (pthread_create(&sid, NULL, sensor_thread, &app) != 0) {
        fprintf(stderr, "sensor pthread failed\n");
        return 1;
    }

    (void)ev_loop_run(&owner);

    (void)pthread_join(sid, NULL);
    ev_loop_stop(&worker);
    (void)pthread_join(wid, NULL);

    printf("=== done sync=%d post=%d alarm=%d dropped=%u ===\n",
           app.sync_hits, app.post_hits, app.alarm_hits, bus.post_dropped);

    (void)ev_bus_unsubscribe(&bus, id_sync);
    (void)ev_bus_unsubscribe(&bus, id_post);
    (void)ev_bus_unsubscribe(&bus, id_alarm);
    ev_bus_deinit(&bus);
    ev_loop_deinit(&worker);
    ev_loop_deinit(&owner);

    return (app.sync_hits == SAMPLE_COUNT && app.post_hits == SAMPLE_COUNT && app.alarm_hits == 1) ? 0 : 1;
}
