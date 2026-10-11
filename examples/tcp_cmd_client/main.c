/**
 * @file    main.c
 * @brief   网络 loop + 工作 loop 双线程演示；session 生命周期经 ev_bus 扇出
 *
 * 读代码: proto.h → session.c → cmd_engine.c → worker.c → cmds.c → main.c
 *
 * 接线顺序:
 *   worker_start → net loop → bus → engine → session
 *   online/offline：app 回调里 publish；SYNC 本线程观察 + POST 到 worker 线程观察
 */
#include "app_topics.h"
#include "cmd_engine.h"
#include "cmds.h"
#include "evkit/ev_bus.h"
#include "evkit/ev_loop.h"
#include "session.h"
#include "worker.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ev_loop_t *g_loop = NULL;

typedef struct {
    cmd_engine_t *engine;
    ev_bus_t *bus;
    int sub_online_sync;
    int sub_online_post;
    int sub_offline_sync;
    uint32_t online_sync_hits;
    uint32_t online_post_hits;
    uint32_t offline_sync_hits;
} app_t;

static const uint32_t k_topic_online = APP_TOPIC_SESSION_ONLINE;
static const uint32_t k_topic_offline = APP_TOPIC_SESSION_OFFLINE;

static void app_on_signal(int sig)
{
    (void)sig;
    if (g_loop != NULL) {
        ev_loop_stop(g_loop);
    }
}

/** SYNC：分发线程（网络 loop）立刻看到上线 */
static void app_bus_on_online_sync(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *t = topic;
    (void)payload;
    app->online_sync_hits++;
    printf("[bus/SYNC] topic=%u session online (net thread)\n", t != NULL ? *t : 0u);
}

/** POST：工作线程观察到上线（payload 用 NULL，避免跨线程捏 session*） */
static void app_bus_on_online_post(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *t = topic;
    (void)payload;
    app->online_post_hits++;
    printf("[bus/POST] topic=%u session online (worker thread)\n", t != NULL ? *t : 0u);
}

static void app_bus_on_offline_sync(const void *topic, void *payload, void *user_data)
{
    app_t *app = user_data;
    const uint32_t *t = topic;
    (void)payload;
    app->offline_sync_hits++;
    printf("[bus/SYNC] topic=%u session offline (net thread)\n", t != NULL ? *t : 0u);
}

static void app_on_online(session_t *s, void *user_data)
{
    app_t *app = user_data;
    printf("[app] online, stats connect_ok=%u\n", session_get_stats(s)->connect_ok);
    /* topic 须活过 POST：用静态 topic，勿用栈上临时变量 */
    int n = ev_bus_publish(app->bus, &k_topic_online, NULL);
    printf("[app] bus publish ONLINE delivered=%d\n", n);
}

static void app_on_offline(session_t *s, void *user_data)
{
    app_t *app = user_data;
    (void)ev_bus_publish(app->bus, &k_topic_offline, NULL);
    cmd_engine_on_offline(s, app->engine);
}

static void app_on_line(session_t *s, const char *line, void *user_data)
{
    app_t *app = user_data;
    cmd_engine_on_line(s, line, app->engine);
}

static void app_print_help(void)
{
    printf("TCP JSON command client (evkit skeleton)\n");
    printf("  sync  : ping, echo, quit  (网络线程)\n");
    printf("  async : slow, count       (工作线程 loop+FSM，经 ev_loop_post 回传)\n");
    printf("  bus   : SESSION_ONLINE/OFFLINE → SYNC(net) + POST(worker)\n");
    printf("Protocol: NDJSON, see proto.h\n");
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    uint16_t port = 9090;
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = (uint16_t)atoi(argv[2]);
    }

    app_print_help();

    if (worker_start() != 0) {
        fprintf(stderr, "worker_start failed\n");
        return 1;
    }
    ev_loop_t *worker_lp = worker_loop();
    if (worker_lp == NULL) {
        fprintf(stderr, "worker_loop missing\n");
        worker_stop();
        return 1;
    }

    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        fprintf(stderr, "ev_loop_init failed\n");
        worker_stop();
        return 1;
    }
    g_loop = &loop;

    ev_bus_t bus;
    if (ev_bus_init(&bus, &loop) != 0) {
        fprintf(stderr, "ev_bus_init failed\n");
        ev_loop_deinit(&loop);
        worker_stop();
        return 1;
    }

    size_t nhandlers = 0;
    const cmd_handler_t *handlers = cmds_handlers(&nhandlers);

    cmd_engine_t engine = {0};
    session_t session = {0};
    app_t app = {0};
    app.engine = &engine;
    app.bus = &bus;

    if (cmd_engine_init(&engine, &session, &loop, handlers, nhandlers) != 0) {
        fprintf(stderr, "cmd_engine_init failed\n");
        ev_bus_deinit(&bus);
        ev_loop_deinit(&loop);
        worker_stop();
        return 1;
    }
    if (session_init(&session, &loop, host, port, app_on_line, app_on_online, app_on_offline, &app) != 0) {
        fprintf(stderr, "session_init failed\n");
        cmd_engine_deinit(&engine);
        ev_bus_deinit(&bus);
        ev_loop_deinit(&loop);
        worker_stop();
        return 1;
    }

    app.sub_online_sync =
        ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, app_bus_on_online_sync, &app, ev_bus_match_u32, &k_topic_online);
    app.sub_online_post =
        ev_bus_subscribe(&bus, EV_BUS_POST, worker_lp, app_bus_on_online_post, &app, ev_bus_match_u32, &k_topic_online);
    app.sub_offline_sync =
        ev_bus_subscribe(&bus, EV_BUS_SYNC, NULL, app_bus_on_offline_sync, &app, ev_bus_match_u32, &k_topic_offline);
    if (app.sub_online_sync < 0 || app.sub_online_post < 0 || app.sub_offline_sync < 0) {
        fprintf(stderr, "bus subscribe failed\n");
        session_deinit(&session);
        cmd_engine_deinit(&engine);
        ev_bus_deinit(&bus);
        ev_loop_deinit(&loop);
        worker_stop();
        return 1;
    }

    signal(SIGINT, app_on_signal);
    signal(SIGTERM, app_on_signal);

    printf("[app] net+worker+bus ready; connecting %s:%u\n", host, (unsigned)port);
    session_start(&session);
    (void)ev_loop_run(&loop);

    printf("[app] shutting down\n");
    session_stop(&session);
    /* 先退订，再停业务；worker 上可能仍有已入队 ONLINE POST（payload=NULL，安全） */
    (void)ev_bus_unsubscribe(&bus, app.sub_online_sync);
    (void)ev_bus_unsubscribe(&bus, app.sub_online_post);
    (void)ev_bus_unsubscribe(&bus, app.sub_offline_sync);
    ev_bus_deinit(&bus);
    cmd_engine_deinit(&engine);
    session_deinit(&session);

    const session_stats_t *st = session_get_stats(&session);
    printf("[app] stats: connect_ok=%u fail=%u disc=%u rx=%u tx=%u | bus online_sync=%u online_post=%u offline_sync=%u\n",
           st->connect_ok, st->connect_fail, st->disconnect, st->rx_lines, st->tx_lines, app.online_sync_hits,
           app.online_post_hits, app.offline_sync_hits);

    ev_loop_deinit(&loop);
    worker_stop();
    g_loop = NULL;
    return 0;
}
