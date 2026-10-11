/**
 * @file    main.c
 * @brief   设计示例：多口（模拟 CAN / 485）+ 普通收包（非 FSM）+ 升级会话（FSM）+ 失联
 *
 * ---------------------------------------------------------------------------
 * 怎么拆（推荐）
 * ---------------------------------------------------------------------------
 *   port 层（每口一份，非 FSM）
 *     - ev_io 收字节 → 行/帧组装（几个变量即可）
 *     - idle timer：超过 LINK_IDLE_MS 无合法帧 → 失联；升级中则 post EV_LINK_LOST
 *     - 拼出一帧后交给 dispatcher
 *
 *   dispatcher（非 FSM）
 *     - 普通业务帧 → on_telemetry
 *     - 升级帧且会话进行中 → ev_fsm_post(&upgrade_fsm, …)
 *
 *   upgrade_fsm（要 FSM）
 *     IDLE → HANDSHAKE → XFER → VERIFY → DONE
 *     TIMEOUT / LINK_LOST / NACK → ABORT
 *
 * 普通接收解析一般不必做成 ev_fsm：它是流式拼帧，不是多阶段业务会话。
 * 失联用「最后收包时间 + 周期 idle timer」即可，不必单独一台 FSM。
 *
 * CAN 与 485：同一套帧回调 / 同一台 upgrade_fsm；差别只在 port 发帧实现。
 * 本例用 socketpair 模拟对端；真实项目换成 SocketCAN / termios。
 *
 * 运行：make upgrade-link && ./upgrade_link [can|rs485]
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum {
    FRAME_TELEMETRY = 1,
    FRAME_HELLO_ACK,
    FRAME_CHUNK_ACK,
    FRAME_VERIFY_OK,
    FRAME_NACK
} frame_type_t;

typedef struct {
    frame_type_t type;
    int seq;
} frame_t;

typedef struct app app_t;

typedef struct {
    const char *name;
    app_t *app;
    ev_loop_t *loop;
    ev_io_t io;
    ev_timer_t idle_timer;
    int fd;
    char rx[128];
    size_t rx_len;
    uint64_t last_rx_ms;
    int link_up;
    int tele_count;
} port_t;

enum {
    ST_IDLE = 0,
    ST_HANDSHAKE,
    ST_XFER,
    ST_VERIFY,
    ST_DONE,
    ST_ABORT,
    ST_COUNT
};

enum {
    EV_UPG_START = 1,
    EV_HELLO_ACK,
    EV_CHUNK_ACK,
    EV_CHUNKS_DONE,
    EV_VERIFY_OK,
    EV_NACK,
    EV_TIMEOUT,
    EV_LINK_LOST
};

#define CHUNK_TOTAL     3
#define UPG_TIMEOUT_MS  2000u
#define LINK_IDLE_MS    1500u
#define TELE_BEFORE_UPG 3

struct app {
    ev_loop_t *loop;
    port_t *active;
    port_t can;
    port_t rs485;
    ev_fsm_t upg;
    ev_timer_t upg_timer;
    int chunk_sent;
    int chunk_acked;
    int upgrade_done;
    int aborted;
};

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return (flags < 0) ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void port_send_line(port_t *p, const char *line)
{
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "%s\n", line);
    if (n <= 0) {
        return;
    }
    if (write(p->fd, buf, (size_t)n) < 0 && errno != EAGAIN) {
        fprintf(stderr, "[%s] write: %s\n", p->name, strerror(errno));
    }
}

static int parse_frame(const char *line, frame_t *out)
{
    if (strncmp(line, "TELE:", 5) == 0) {
        out->type = FRAME_TELEMETRY;
        out->seq = atoi(line + 5);
        return 0;
    }
    if (strcmp(line, "HELLO_ACK") == 0) {
        out->type = FRAME_HELLO_ACK;
        return 0;
    }
    if (strncmp(line, "ACK:", 4) == 0) {
        out->type = FRAME_CHUNK_ACK;
        out->seq = atoi(line + 4);
        return 0;
    }
    if (strcmp(line, "VERIFY_OK") == 0) {
        out->type = FRAME_VERIFY_OK;
        return 0;
    }
    if (strcmp(line, "NACK") == 0) {
        out->type = FRAME_NACK;
        return 0;
    }
    return -1;
}

static void upg_post(app_t *app, int event)
{
    if (ev_fsm_post(&app->upg, event, NULL) != 0) {
        fprintf(stderr, "[upg] post overflow event=%d\n", event);
    }
}

static void dispatcher_on_frame(port_t *p, const frame_t *fr);
static void upg_arm_timeout(app_t *app);

static void port_on_idle(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    port_t *p = timer->user_data;
    if (!p->link_up) {
        return;
    }
    uint64_t now = ev_loop_now_ms_u64();
    if (now - p->last_rx_ms < LINK_IDLE_MS) {
        return;
    }
    printf("[%s] LINK LOST (idle >= %ums)\n", p->name, LINK_IDLE_MS);
    p->link_up = 0;
    int st = ev_fsm_get_state(&p->app->upg);
    if (p->app->active == p && st != ST_IDLE && st != ST_DONE && st != ST_ABORT) {
        upg_post(p->app, EV_LINK_LOST);
    }
}

static void port_note_rx(port_t *p)
{
    p->last_rx_ms = ev_loop_now_ms_u64();
    if (!p->link_up) {
        p->link_up = 1;
        printf("[%s] link UP\n", p->name);
    }
}

static void port_feed(port_t *p, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            p->rx[p->rx_len] = '\0';
            frame_t fr;
            if (parse_frame(p->rx, &fr) == 0) {
                port_note_rx(p);
                printf("[%s] <- %s\n", p->name, p->rx);
                dispatcher_on_frame(p, &fr);
            }
            p->rx_len = 0;
            continue;
        }
        if (p->rx_len + 1 < sizeof(p->rx) && data[i] != '\r') {
            p->rx[p->rx_len++] = data[i];
        } else {
            p->rx_len = 0;
        }
    }
}

static void port_on_io(ev_loop_t *loop, ev_io_t *io, uint32_t events)
{
    (void)loop;
    port_t *p = io->user_data;
    if (!(events & EV_LOOP_READ)) {
        return;
    }
    for (;;) {
        char buf[64];
        ssize_t n = read(p->fd, buf, sizeof(buf));
        if (n > 0) {
            port_feed(p, buf, (size_t)n);
            continue;
        }
        if (n == 0) {
            printf("[%s] peer EOF → remove io\n", p->name);
            p->link_up = 0;
            (void)ev_io_remove(p->loop, &p->io);
            if (p->app->active == p) {
                upg_post(p->app, EV_LINK_LOST);
            }
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        fprintf(stderr, "[%s] read: %s\n", p->name, strerror(errno));
        return;
    }
}

static int port_init(port_t *p, app_t *app, const char *name, int fd)
{
    memset(p, 0, sizeof(*p));
    p->name = name;
    p->app = app;
    p->loop = app->loop;
    p->fd = fd;
    if (set_nonblock(fd) != 0) {
        return -1;
    }
    p->io.fd = fd;
    p->io.events = EV_LOOP_READ;
    p->io.callback = port_on_io;
    p->io.user_data = p;
    if (ev_io_add(p->loop, &p->io) != 0) {
        return -1;
    }
    ev_timer_start(p->loop, &p->idle_timer, LINK_IDLE_MS, LINK_IDLE_MS, port_on_idle, p);
    return 0;
}

static void port_deinit(port_t *p)
{
    (void)ev_timer_remove(&p->idle_timer);
    (void)ev_io_remove(p->loop, &p->io);
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
}

static void maybe_start_upgrade(app_t *app, port_t *p)
{
    if (app->upgrade_done || app->aborted) {
        return;
    }
    if (ev_fsm_get_state(&app->upg) != ST_IDLE) {
        return;
    }
    if (p->tele_count < TELE_BEFORE_UPG) {
        return;
    }
    app->active = p;
    printf("[app] telemetry enough on %s → start upgrade\n", p->name);
    upg_post(app, EV_UPG_START);
}

static void dispatcher_on_frame(port_t *p, const frame_t *fr)
{
    app_t *app = p->app;
    int st = ev_fsm_get_state(&app->upg);

    if (fr->type == FRAME_TELEMETRY) {
        p->tele_count++;
        printf("[%s] telemetry #%d seq=%d\n", p->name, p->tele_count, fr->seq);
        maybe_start_upgrade(app, p);
        return;
    }

    if (st == ST_IDLE || st == ST_DONE || st == ST_ABORT) {
        printf("[%s] drop upgrade frame (fsm not active)\n", p->name);
        return;
    }
    if (app->active != p) {
        printf("[%s] drop upgrade frame (not active port)\n", p->name);
        return;
    }

    switch (fr->type) {
    case FRAME_HELLO_ACK:
        upg_post(app, EV_HELLO_ACK);
        break;
    case FRAME_CHUNK_ACK:
        if (fr->seq == app->chunk_sent) {
            upg_post(app, EV_CHUNK_ACK);
        }
        break;
    case FRAME_VERIFY_OK:
        upg_post(app, EV_VERIFY_OK);
        break;
    case FRAME_NACK:
        upg_post(app, EV_NACK);
        break;
    default:
        break;
    }
}

static void on_upg_timeout(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    app_t *app = timer->user_data;
    printf("[upg] TIMEOUT in %s\n", ev_fsm_state_name(&app->upg));
    upg_post(app, EV_TIMEOUT);
}

static void upg_arm_timeout(app_t *app)
{
    ev_timer_start(app->loop, &app->upg_timer, UPG_TIMEOUT_MS, 0, on_upg_timeout, app);
}

static void upg_disarm_timeout(app_t *app)
{
    (void)ev_timer_stop(&app->upg_timer);
}

static void action_start(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    app->chunk_sent = 0;
    app->chunk_acked = 0;
    printf("[upg] HANDSHAKE → HELLO on %s\n", app->active->name);
    port_send_line(app->active, "HELLO");
    upg_arm_timeout(app);
}

static void action_hello_ok(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    app->chunk_sent = 1;
    char line[32];
    snprintf(line, sizeof(line), "CHUNK:%d:data", app->chunk_sent);
    printf("[upg] XFER → %s\n", line);
    port_send_line(app->active, line);
    upg_arm_timeout(app);
}

static void action_chunk_ack(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    app->chunk_acked = app->chunk_sent;
    if (app->chunk_acked >= CHUNK_TOTAL) {
        printf("[upg] chunks complete\n");
        upg_post(app, EV_CHUNKS_DONE);
        return;
    }
    app->chunk_sent++;
    char line[32];
    snprintf(line, sizeof(line), "CHUNK:%d:data", app->chunk_sent);
    printf("[upg] XFER → %s\n", line);
    port_send_line(app->active, line);
    upg_arm_timeout(app);
}

static void action_enter_verify(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    printf("[upg] VERIFY\n");
    port_send_line(app->active, "VERIFY");
    upg_arm_timeout(app);
}

static void action_done(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    app_t *app = user_data;
    upg_disarm_timeout(app);
    app->upgrade_done = 1;
    printf("[upg] DONE on %s\n", app->active->name);
    ev_loop_stop(app->loop);
}

static void action_abort(void *user_data, int event, void *arg)
{
    (void)arg;
    app_t *app = user_data;
    upg_disarm_timeout(app);
    app->aborted = 1;
    printf("[upg] ABORT (event=%d)\n", event);
    ev_loop_stop(app->loop);
}

static void action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    action_abort(user_data, EV_FSM_FAULT, NULL);
}

static const ev_fsm_transition_t g_upg_tr[] = {
    { ST_IDLE, EV_UPG_START, ST_HANDSHAKE, action_start },
    { ST_HANDSHAKE, EV_HELLO_ACK, ST_XFER, action_hello_ok },
    { ST_XFER, EV_CHUNK_ACK, EV_FSM_STAY, action_chunk_ack },
    { ST_XFER, EV_CHUNKS_DONE, ST_VERIFY, action_enter_verify },
    { ST_VERIFY, EV_VERIFY_OK, ST_DONE, action_done },
    { ST_HANDSHAKE, EV_TIMEOUT, ST_ABORT, action_abort },
    { ST_XFER, EV_TIMEOUT, ST_ABORT, action_abort },
    { ST_VERIFY, EV_TIMEOUT, ST_ABORT, action_abort },
    { ST_HANDSHAKE, EV_NACK, ST_ABORT, action_abort },
    { ST_XFER, EV_NACK, ST_ABORT, action_abort },
    { ST_VERIFY, EV_NACK, ST_ABORT, action_abort },
    { ST_HANDSHAKE, EV_LINK_LOST, ST_ABORT, action_abort },
    { ST_XFER, EV_LINK_LOST, ST_ABORT, action_abort },
    { ST_VERIFY, EV_LINK_LOST, ST_ABORT, action_abort },
    { EV_FSM_ANY, EV_FSM_FAULT, ST_ABORT, action_fault },
};

static const ev_fsm_state_t g_upg_st[ST_COUNT] = {
    [ST_IDLE] = { .name = "IDLE" },
    [ST_HANDSHAKE] = { .name = "HANDSHAKE" },
    [ST_XFER] = { .name = "XFER" },
    [ST_VERIFY] = { .name = "VERIFY" },
    [ST_DONE] = { .name = "DONE" },
    [ST_ABORT] = { .name = "ABORT" },
};

typedef struct {
    int fd;
} peer_arg_t;

static void *peer_main(void *arg)
{
    peer_arg_t *pa = arg;
    int fd = pa->fd;
    char buf[128];
    size_t len = 0;

    for (int i = 1; i <= TELE_BEFORE_UPG; i++) {
        usleep(80 * 1000);
        dprintf(fd, "TELE:%d\n", i);
    }

    for (;;) {
        char tmp[64];
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                usleep(5 * 1000);
                continue;
            }
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            if (tmp[i] == '\n') {
                buf[len] = '\0';
                len = 0;
                if (strcmp(buf, "HELLO") == 0) {
                    usleep(30 * 1000);
                    dprintf(fd, "HELLO_ACK\n");
                } else if (strncmp(buf, "CHUNK:", 6) == 0) {
                    int idx = atoi(buf + 6);
                    usleep(30 * 1000);
                    dprintf(fd, "ACK:%d\n", idx);
                } else if (strcmp(buf, "VERIFY") == 0) {
                    usleep(30 * 1000);
                    dprintf(fd, "VERIFY_OK\n");
                }
                continue;
            }
            if (len + 1 < sizeof(buf)) {
                buf[len++] = tmp[i];
            } else {
                len = 0;
            }
        }
    }
    close(fd);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *which = "can";
    if (argc >= 2) {
        which = argv[1];
    }
    int use_can = (strcmp(which, "rs485") != 0);

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== upgrade_link: active=%s ===\n", use_can ? "can" : "rs485");
    printf("port=拼帧+失联(非FSM); upgrade=ev_fsm; can/rs485 共用会话机\n");

    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        return 1;
    }

    int can_sv[2];
    int rs_sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, can_sv) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, rs_sv) != 0) {
        perror("socketpair");
        return 1;
    }

    app_t app = {0};
    app.loop = &loop;

    /* 两口都建好；未演示的口也对端保持打开（安静），避免 EOF 刷屏 */
    if (port_init(&app.can, &app, "can0", can_sv[0]) != 0 || port_init(&app.rs485, &app, "rs485", rs_sv[0]) != 0) {
        fprintf(stderr, "port_init failed\n");
        return 1;
    }
    app.active = use_can ? &app.can : &app.rs485;

    if (ev_fsm_init(&app.upg, ST_IDLE, g_upg_tr, sizeof(g_upg_tr) / sizeof(g_upg_tr[0]), g_upg_st, ST_COUNT, &app, NULL) != 0) {
        fprintf(stderr, "upg fsm init failed\n");
        return 1;
    }
    if (ev_loop_register_fsm(&loop, &app.upg) != 0) {
        return 1;
    }

    peer_arg_t peer = { .fd = use_can ? can_sv[1] : rs_sv[1] };
    int idle_peer = use_can ? rs_sv[1] : can_sv[1];
    (void)set_nonblock(peer.fd);
    (void)set_nonblock(idle_peer);

    pthread_t tid;
    if (pthread_create(&tid, NULL, peer_main, &peer) != 0) {
        return 1;
    }

    (void)ev_loop_run(&loop);

    /* 先关本端，唤醒 peer 的 read，再 join */
    (void)ev_timer_remove(&app.upg_timer);
    (void)ev_loop_unregister_fsm(&loop, &app.upg);
    ev_fsm_deinit(&app.upg);
    port_deinit(&app.can);
    port_deinit(&app.rs485);
    close(idle_peer);
    (void)pthread_join(tid, NULL);
    ev_loop_deinit(&loop);

    printf("=== done upgrade_done=%d aborted=%d ===\n", app.upgrade_done, app.aborted);
    return app.upgrade_done ? 0 : 1;
}
