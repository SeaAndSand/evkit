/**
 * @file    main.c
 * @brief   真实 TCP 会话：非阻塞 connect + FSM 重连 + 心跳
 *
 * 回调只 ev_fsm_post；loop 回合末统一 poll。
 * 依赖：python3 examples/tcp_reconnect/echo_server.py [port]
 *
 *   IDLE ──START──► CONNECTING ──CONNECT_OK──► ONLINE
 *                      │                          │
 *                      └──CONNECT_FAIL/LOST──► WAIT_RETRY ──RETRY──► CONNECTING
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    ST_IDLE = 0,
    ST_CONNECTING,
    ST_ONLINE,
    ST_WAIT_RETRY,
    ST_COUNT
};

enum {
    EV_START = 1,
    EV_CONNECT_OK,
    EV_CONNECT_FAIL,
    EV_LOST,
    EV_RETRY,
    EV_PONG,
    EV_STOP
};

#define CONNECT_TIMEOUT_MS 3000u
#define RETRY_MS           1000u
#define HEARTBEAT_MS       1000u
#define TARGET_PONGS       3

typedef struct {
    ev_loop_t *loop;
    ev_fsm_t fsm;
    ev_io_t io;
    ev_timer_t connect_timer;
    ev_timer_t retry_timer;
    ev_timer_t heartbeat_timer;
    const char *host;
    const char *port;
    int fd;
    char rx[256];
    size_t rx_len;
    int pong_count;
    int stop_after_pongs;
} session_t;

static ev_loop_t *g_loop;

static void session_post(session_t *s, int event)
{
    if (ev_fsm_post(&s->fsm, event, NULL) != 0) {
        fprintf(stderr, "[tcp] fsm overflow event=%d dropped=%u\n", event, s->fsm.post_dropped);
    }
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void session_close_fd(session_t *s)
{
    if (s->fd < 0) {
        return;
    }
    (void)ev_io_remove(s->loop, &s->io);
    close(s->fd);
    s->fd = -1;
    s->rx_len = 0;
    memset(&s->io, 0, sizeof(s->io));
}

static void on_signal(int sig)
{
    (void)sig;
    if (g_loop != NULL) {
        ev_loop_stop(g_loop);
    }
}

static void on_connect_timeout(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *s = timer->user_data;
    fprintf(stderr, "[tcp] connect timeout\n");
    session_post(s, EV_CONNECT_FAIL);
}

static void on_retry_timer(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *s = timer->user_data;
    session_post(s, EV_RETRY);
}

static void on_heartbeat(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *s = timer->user_data;
    if (s->fd < 0) {
        return;
    }
    const char *msg = "ping\n";
    ssize_t n = write(s->fd, msg, strlen(msg));
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        fprintf(stderr, "[tcp] heartbeat write fail: %s\n", strerror(errno));
        session_post(s, EV_LOST);
        return;
    }
    printf("[tcp] -> ping\n");
}

static void session_feed_rx(session_t *s, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (s->rx_len + 1 >= sizeof(s->rx)) {
            s->rx_len = 0;
            continue;
        }
        if (data[i] == '\n') {
            s->rx[s->rx_len] = '\0';
            printf("[tcp] <- %s\n", s->rx);
            if (strcmp(s->rx, "pong") == 0) {
                session_post(s, EV_PONG);
            } else if (strcmp(s->rx, "welcome") == 0) {
                /* ignore */
            }
            s->rx_len = 0;
            continue;
        }
        if (data[i] != '\r') {
            s->rx[s->rx_len++] = data[i];
        }
    }
}

static void on_socket_io(ev_loop_t *loop, ev_io_t *io, uint32_t events)
{
    (void)loop;
    session_t *s = io->user_data;

    if (ev_fsm_get_state(&s->fsm) == ST_CONNECTING) {
        if (events & EV_LOOP_WRITE) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                fprintf(stderr, "[tcp] connect fail: %s\n", strerror(err != 0 ? err : errno));
                session_post(s, EV_CONNECT_FAIL);
                return;
            }
            session_post(s, EV_CONNECT_OK);
        }
        return;
    }

    if (events & EV_LOOP_READ) {
        for (;;) {
            char buf[128];
            ssize_t n = read(s->fd, buf, sizeof(buf));
            if (n > 0) {
                session_feed_rx(s, buf, (size_t)n);
                continue;
            }
            if (n == 0) {
                printf("[tcp] peer closed\n");
                session_post(s, EV_LOST);
                return;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            fprintf(stderr, "[tcp] read: %s\n", strerror(errno));
            session_post(s, EV_LOST);
            return;
        }
    }
}

static int session_begin_connect(session_t *s)
{
    session_close_fd(s);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(s->host, s->port, &hints, &res);
    if (gai != 0) {
        fprintf(stderr, "[tcp] getaddrinfo: %s\n", gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    int in_progress = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (set_nonblock(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            in_progress = 0;
            break;
        }
        if (errno == EINPROGRESS) {
            in_progress = 1;
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return -1;
    }

    s->fd = fd;
    s->io.fd = fd;
    s->io.events = in_progress ? EV_LOOP_WRITE : (EV_LOOP_READ | EV_LOOP_WRITE);
    s->io.callback = on_socket_io;
    s->io.user_data = s;
    if (ev_io_add(s->loop, &s->io) != 0) {
        close(fd);
        s->fd = -1;
        return -1;
    }

    ev_timer_start(s->loop, &s->connect_timer, CONNECT_TIMEOUT_MS, 0, on_connect_timeout, s);

    if (!in_progress) {
        /* 本机立刻连上：下一回合用 WRITE/可读路径或直接 post */
        session_post(s, EV_CONNECT_OK);
    }
    return 0;
}

static void enter_connecting(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    printf("[fsm] -> CONNECTING %s:%s\n", s->host, s->port);
    if (session_begin_connect(s) != 0) {
        session_post(s, EV_CONNECT_FAIL);
    }
}

static void exit_connecting(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    (void)ev_timer_stop(&s->connect_timer);
}

static void enter_online(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    printf("[fsm] -> ONLINE\n");
    if (ev_io_mod(s->loop, &s->io, EV_LOOP_READ) != 0) {
        session_post(s, EV_LOST);
        return;
    }
    /* 周期心跳：完成时 now+周期（会漂，对本例足够） */
    ev_timer_start(s->loop, &s->heartbeat_timer, 200, HEARTBEAT_MS, on_heartbeat, s);
}

static void exit_online(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    (void)ev_timer_stop(&s->heartbeat_timer);
    session_close_fd(s);
}

static void enter_wait_retry(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    printf("[fsm] -> WAIT_RETRY (%ums)\n", RETRY_MS);
    session_close_fd(s);
    ev_timer_start(s->loop, &s->retry_timer, RETRY_MS, 0, on_retry_timer, s);
}

static void exit_wait_retry(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    (void)ev_timer_stop(&s->retry_timer);
}

static void action_pong(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    session_t *s = user_data;
    s->pong_count++;
    printf("[tcp] pong_count=%d/%d\n", s->pong_count, s->stop_after_pongs);
    if (s->stop_after_pongs > 0 && s->pong_count >= s->stop_after_pongs) {
        session_post(s, EV_STOP);
    }
}

static void action_stop(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    session_t *s = user_data;
    printf("[fsm] STOP → idle, leave loop\n");
    session_close_fd(s);
    (void)ev_timer_stop(&s->heartbeat_timer);
    (void)ev_timer_stop(&s->retry_timer);
    (void)ev_timer_stop(&s->connect_timer);
    ev_loop_stop(s->loop);
}

static void action_nop(void *user_data, int event, void *arg)
{
    (void)user_data;
    (void)event;
    (void)arg;
}

static void action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    session_t *s = user_data;
    fprintf(stderr, "[fsm] FAULT hits=%u → IDLE\n", s->fsm.poll_limit_hits);
    session_close_fd(s);
}

static const ev_fsm_transition_t g_transitions[] = {
    { ST_IDLE, EV_START, ST_CONNECTING, action_nop },
    { ST_CONNECTING, EV_CONNECT_OK, ST_ONLINE, action_nop },
    { ST_CONNECTING, EV_CONNECT_FAIL, ST_WAIT_RETRY, action_nop },
    { ST_CONNECTING, EV_LOST, ST_WAIT_RETRY, action_nop },
    { ST_CONNECTING, EV_STOP, ST_IDLE, action_stop },
    { ST_ONLINE, EV_LOST, ST_WAIT_RETRY, action_nop },
    { ST_ONLINE, EV_PONG, EV_FSM_STAY, action_pong },
    { ST_ONLINE, EV_STOP, ST_IDLE, action_stop },
    { ST_WAIT_RETRY, EV_RETRY, ST_CONNECTING, action_nop },
    { ST_WAIT_RETRY, EV_STOP, ST_IDLE, action_stop },
    { EV_FSM_ANY, EV_FSM_FAULT, ST_IDLE, action_fault },
};

static const ev_fsm_state_t g_states[ST_COUNT] = {
    [ST_IDLE] = { .name = "IDLE" },
    [ST_CONNECTING] = { .name = "CONNECTING", .on_enter = enter_connecting, .on_exit = exit_connecting },
    [ST_ONLINE] = { .name = "ONLINE", .on_enter = enter_online, .on_exit = exit_online },
    [ST_WAIT_RETRY] = { .name = "WAIT_RETRY", .on_enter = enter_wait_retry, .on_exit = exit_wait_retry },
};

static void on_boot(void *user_data)
{
    session_t *s = user_data;
    session_post(s, EV_START);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *port = "9100";
    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = argv[2];
    }

    printf("=== tcp_reconnect: %s:%s (need echo_server.py) ===\n", host, port);

    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        fprintf(stderr, "loop init failed\n");
        return 1;
    }
    g_loop = &loop;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    session_t session = {0};
    session.loop = &loop;
    session.host = host;
    session.port = port;
    session.fd = -1;
    session.stop_after_pongs = TARGET_PONGS;

    if (ev_fsm_init(&session.fsm, ST_IDLE, g_transitions, sizeof(g_transitions) / sizeof(g_transitions[0]),
                    g_states, ST_COUNT, &session, NULL) != 0) {
        fprintf(stderr, "fsm init failed\n");
        ev_loop_deinit(&loop);
        return 1;
    }
    if (ev_loop_register_fsm(&loop, &session.fsm) != 0) {
        fprintf(stderr, "register_fsm failed\n");
        ev_fsm_deinit(&session.fsm);
        ev_loop_deinit(&loop);
        return 1;
    }

    (void)ev_loop_post(&loop, on_boot, &session);
    (void)ev_loop_run(&loop);

    (void)ev_timer_remove(&session.connect_timer);
    (void)ev_timer_remove(&session.retry_timer);
    (void)ev_timer_remove(&session.heartbeat_timer);
    session_close_fd(&session);
    (void)ev_loop_unregister_fsm(&loop, &session.fsm);
    ev_fsm_deinit(&session.fsm);
    ev_loop_deinit(&loop);
    g_loop = NULL;

    printf("=== done pongs=%d ===\n", session.pong_count);
    return (session.pong_count >= TARGET_PONGS) ? 0 : 1;
}
