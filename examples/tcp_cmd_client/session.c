/**
 * @file    session.c
 * @brief   TCP 会话实现：非阻塞 connect / 读写续传 / 重连
 *
 * ONLINE 读写:
 *   READ  → drain socket 进 recv_buf → 按行回调 on_line
 *   WRITE → 尽量把 send_buf write 出去；EAGAIN 则订 EPOLLOUT，发完退订
 */
#define _GNU_SOURCE
#include "session.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
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
    EV_DISCONNECTED,
    EV_RETRY,
    EV_STOP
};

/* session_open_socket 的返回值 */
typedef enum {
    CONNECT_FAILED = -1,
    CONNECT_IN_PROGRESS = 0,
    CONNECT_DONE = 1
} connect_result_t;

static void session_post_event(session_t *s, int event)
{
    if (ev_fsm_post(&s->fsm, event, NULL) != 0) {
        fprintf(stderr, "[session] fsm post overflow event=%d dropped=%u\n", event, s->fsm.post_dropped);
    }
}

static void session_close_socket(session_t *s)
{
    if (s->fd >= 0) {
        if (s->io.callback != NULL) {
            (void)ev_io_remove(s->loop, &s->io);
        }
        close(s->fd);
        s->fd = -1;
    }
    s->write_pending = 0;
    net_buf_clear(&s->recv_buf);
    net_buf_clear(&s->send_buf);
}

static int session_set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int session_update_io_interest(session_t *s)
{
    if (s->fd < 0) {
        return -1;
    }
    uint32_t ev = EV_LOOP_READ;
    if (s->write_pending || ev_fsm_get_state(&s->fsm) == ST_CONNECTING) {
        ev |= EV_LOOP_WRITE;
    }
    return ev_io_mod(s->loop, &s->io, ev);
}

/**
 * 把 send_buf 尽量 write 出去。
 * - 发不动 (EAGAIN)：订 EPOLLOUT，等下次可写再来
 * - 发完：退订 EPOLLOUT
 * - 出错：断开
 */
static void session_flush_send_buf(session_t *s)
{
    while (net_buf_len(&s->send_buf) > 0) {
        ssize_t n = write(s->fd, s->send_buf.data, s->send_buf.len);
        if (n > 0) {
            net_buf_consume(&s->send_buf, (size_t)n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            s->write_pending = 1;
            (void)session_update_io_interest(s);
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        s->stats.write_err++;
        session_post_event(s, EV_DISCONNECTED);
        return;
    }
    if (s->write_pending) {
        s->write_pending = 0;
        (void)session_update_io_interest(s);
    }
}

/** 读到 EAGAIN / 对端关闭 / 出错为止；字节进 recv_buf。返回 0 继续，-1 已断 */
static int session_drain_socket(session_t *s)
{
    for (;;) {
        uint8_t tmp[4096] = {0};
        ssize_t n = read(s->fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (net_buf_append(&s->recv_buf, tmp, (size_t)n) != 0) {
                s->stats.rx_overflow++;
                fprintf(stderr, "[session] rx overflow → disconnect\n");
                session_post_event(s, EV_DISCONNECTED);
                return -1;
            }
            continue;
        }
        if (n == 0) {
            session_post_event(s, EV_DISCONNECTED);
            return -1;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        if (errno == EINTR) {
            continue;
        }
        session_post_event(s, EV_DISCONNECTED);
        return -1;
    }
}

/** 从 recv_buf 取出完整行并回调；行过长则断开。返回 0 继续，-1 已断 */
static int session_dispatch_lines(session_t *s)
{
    char line[PROTO_MAX_LINE + 1] = {0};
    for (;;) {
        int rc = net_buf_take_line(&s->recv_buf, line, sizeof(line), PROTO_MAX_LINE);
        if (rc == 0) {
            return 0;
        }
        if (rc < 0) {
            s->stats.line_too_long++;
            fprintf(stderr, "[session] line too long → disconnect\n");
            session_post_event(s, EV_DISCONNECTED);
            return -1;
        }
        s->stats.rx_lines++;
        if (s->on_line != NULL) {
            s->on_line(s, line, s->user_data);
        }
        if (ev_fsm_get_state(&s->fsm) != ST_ONLINE) {
            return -1;
        }
    }
}

static void session_on_socket_readable(session_t *s)
{
    if (session_drain_socket(s) != 0) {
        return;
    }
    (void)session_dispatch_lines(s);
}

static void session_on_socket_connecting(session_t *s, uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        session_post_event(s, EV_CONNECT_FAIL);
        return;
    }
    if (events & (EV_LOOP_WRITE | EV_LOOP_READ)) {
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            session_post_event(s, EV_CONNECT_FAIL);
            return;
        }
        session_post_event(s, EV_CONNECT_OK);
        return;
    }
}

static void session_on_socket_online(session_t *s, uint32_t events)
{
    if (events & (EPOLLERR | EPOLLHUP)) {
        session_post_event(s, EV_DISCONNECTED);
        return;
    }
    if (events & EV_LOOP_READ) {
        session_on_socket_readable(s);
        if (ev_fsm_get_state(&s->fsm) != ST_ONLINE) {
            return;
        }
    }
    if (events & EV_LOOP_WRITE) {
        session_flush_send_buf(s);
    }
}

static void session_on_socket_io(ev_loop_t *loop, ev_io_t *io, uint32_t events)
{
    (void)loop;
    session_t *s = io->user_data;
    switch (ev_fsm_get_state(&s->fsm)) {
    case ST_CONNECTING:
        session_on_socket_connecting(s, events);
        break;
    case ST_ONLINE:
        session_on_socket_online(s, events);
        break;
    default:
        break;
    }
}

static void session_on_connect_timeout(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *s = timer->user_data;
    if (ev_fsm_get_state(&s->fsm) == ST_CONNECTING) {
        fprintf(stderr, "[session] connect timeout\n");
        session_post_event(s, EV_CONNECT_FAIL);
    }
}

static void session_on_retry_timer(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *s = timer->user_data;
    session_post_event(s, EV_RETRY);
}

/**
 * 发起非阻塞 connect 并挂上 epoll。
 * CONNECT_DONE         — 本机立刻连上（罕见）
 * CONNECT_IN_PROGRESS  — 等 EPOLLOUT + SO_ERROR
 * CONNECT_FAILED       — 建连失败
 */
static connect_result_t session_open_socket(session_t *s)
{
    session_close_socket(s);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portstr[16] = {0};
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)s->port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(s->host, portstr, &hints, &res);
    if (gai != 0 || res == NULL) {
        fprintf(stderr, "[session] getaddrinfo(%s): %s\n", s->host, gai != 0 ? gai_strerror(gai) : "null");
        return CONNECT_FAILED;
    }

    int fd = -1;
    connect_result_t result = CONNECT_FAILED;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (session_set_nonblock(fd) != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0) {
            result = CONNECT_DONE;
            break;
        }
        if (rc < 0 && errno == EINPROGRESS) {
            result = CONNECT_IN_PROGRESS;
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        return CONNECT_FAILED;
    }

    s->fd = fd;
    s->io = (ev_io_t){ .fd = fd, .events = EV_LOOP_READ | EV_LOOP_WRITE, .callback = session_on_socket_io, .user_data = s };
    if (ev_io_add(s->loop, &s->io) != 0) {
        session_close_socket(s);
        return CONNECT_FAILED;
    }

    ev_timer_start(s->loop, &s->connect_timer, PROTO_CONNECT_TIMEOUT_MS, 0, session_on_connect_timeout, s);
    return result;
}

static void session_enter_connecting(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    printf("[session] → CONNECTING %s:%u\n", s->host, (unsigned)s->port);

    connect_result_t rc = session_open_socket(s);
    if (rc == CONNECT_FAILED) {
        session_post_event(s, EV_CONNECT_FAIL);
    } else if (rc == CONNECT_DONE) {
        session_post_event(s, EV_CONNECT_OK);
    }
    /* CONNECT_IN_PROGRESS：等 session_on_socket_connecting */
}

static void session_exit_connecting(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    (void)ev_timer_stop(&s->connect_timer);
}

static void session_enter_online(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    s->stats.connect_ok++;
    s->write_pending = (net_buf_len(&s->send_buf) > 0);
    (void)session_update_io_interest(s);
    printf("[session] → ONLINE\n");
    if (s->on_online != NULL) {
        s->on_online(s, s->user_data);
    }
}

static void session_exit_online(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    session_close_socket(s);
    s->stats.disconnect++;
    if (s->on_offline != NULL) {
        s->on_offline(s, s->user_data);
    }
}

static void session_enter_wait_retry(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    if (s->stop_requested) {
        session_post_event(s, EV_STOP);
        return;
    }
    s->stats.reconnect++;
    printf("[session] → WAIT_RETRY (%u ms)\n", PROTO_RECONNECT_MS);
    ev_timer_start(s->loop, &s->retry_timer, PROTO_RECONNECT_MS, 0, session_on_retry_timer, s);
}

static void session_exit_wait_retry(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    (void)ev_timer_stop(&s->retry_timer);
}

static void session_enter_idle(void *user_data, int state)
{
    (void)state;
    session_t *s = user_data;
    session_close_socket(s);
    printf("[session] → IDLE\n");
}

static void session_action_connect_fail(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    session_t *s = user_data;
    s->stats.connect_fail++;
    session_close_socket(s);
    fprintf(stderr, "[session] connect failed\n");
}

static const ev_fsm_transition_t k_edges[] = {
    { ST_IDLE, EV_START, ST_CONNECTING, NULL },
    { ST_CONNECTING, EV_CONNECT_OK, ST_ONLINE, NULL },
    { ST_CONNECTING, EV_CONNECT_FAIL, ST_WAIT_RETRY, session_action_connect_fail },
    { ST_CONNECTING, EV_DISCONNECTED, ST_WAIT_RETRY, session_action_connect_fail },
    { ST_CONNECTING, EV_STOP, ST_IDLE, NULL },
    { ST_ONLINE, EV_DISCONNECTED, ST_WAIT_RETRY, NULL },
    { ST_ONLINE, EV_STOP, ST_IDLE, NULL },
    { ST_WAIT_RETRY, EV_RETRY, ST_CONNECTING, NULL },
    { ST_WAIT_RETRY, EV_STOP, ST_IDLE, NULL },
    { EV_FSM_ANY, EV_STOP, ST_IDLE, NULL },
    { EV_FSM_ANY, EV_FSM_FAULT, ST_IDLE, NULL },
};

static const ev_fsm_state_t k_states[ST_COUNT] = {
    [ST_IDLE] = { .name = "IDLE", .on_enter = session_enter_idle, .on_exit = NULL },
    [ST_CONNECTING] = { .name = "CONNECTING", .on_enter = session_enter_connecting, .on_exit = session_exit_connecting },
    [ST_ONLINE] = { .name = "ONLINE", .on_enter = session_enter_online, .on_exit = session_exit_online },
    [ST_WAIT_RETRY] = { .name = "WAIT_RETRY", .on_enter = session_enter_wait_retry, .on_exit = session_exit_wait_retry },
};

int session_init(session_t *s, ev_loop_t *loop, const char *host, uint16_t port, session_line_fn on_line, session_online_fn on_online, session_offline_fn on_offline, void *user_data)
{
    if (s == NULL || loop == NULL || host == NULL) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->loop = loop;
    s->fd = -1;
    s->port = port;
    snprintf(s->host, sizeof(s->host), "%s", host);
    s->on_line = on_line;
    s->on_online = on_online;
    s->on_offline = on_offline;
    s->user_data = user_data;

    if (net_buf_init(&s->recv_buf, PROTO_RX_CAPACITY) != 0) {
        return -1;
    }
    if (net_buf_init(&s->send_buf, PROTO_TX_CAPACITY) != 0) {
        net_buf_deinit(&s->recv_buf);
        return -1;
    }

    if (ev_fsm_init(&s->fsm, ST_IDLE, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, ST_COUNT, s, NULL) != 0) {
        net_buf_deinit(&s->send_buf);
        net_buf_deinit(&s->recv_buf);
        return -1;
    }
    if (ev_loop_register_fsm(loop, &s->fsm) != 0) {
        net_buf_deinit(&s->send_buf);
        net_buf_deinit(&s->recv_buf);
        return -1;
    }
    return 0;
}

void session_deinit(session_t *s)
{
    if (s == NULL) {
        return;
    }
    session_stop(s);
    if (s->loop != NULL) {
        (void)ev_loop_unregister_fsm(s->loop, &s->fsm);
    }
    ev_fsm_deinit(&s->fsm);
    (void)ev_timer_remove(&s->connect_timer);
    (void)ev_timer_remove(&s->retry_timer);
    session_close_socket(s);
    net_buf_deinit(&s->recv_buf);
    net_buf_deinit(&s->send_buf);
}

void session_start(session_t *s)
{
    if (s == NULL) {
        return;
    }
    s->stop_requested = 0;
    session_post_event(s, EV_START);
}

void session_stop(session_t *s)
{
    if (s == NULL) {
        return;
    }
    s->stop_requested = 1;
    session_post_event(s, EV_STOP);
    /* run 已结束后就地 poll，收敛到 IDLE */
    if (s->loop == NULL || !s->loop->running) {
        ev_fsm_poll(&s->fsm);
    }
}

int session_send_line(session_t *s, const char *line)
{
    if (s == NULL || line == NULL) {
        return -1;
    }
    if (ev_fsm_get_state(&s->fsm) != ST_ONLINE || s->fd < 0) {
        return -1;
    }

    size_t len = strlen(line);
    if (len + 1 > net_buf_space(&s->send_buf)) {
        s->stats.tx_overflow++;
        return -1;
    }
    if (net_buf_append(&s->send_buf, line, len) != 0 || net_buf_append(&s->send_buf, "\n", 1) != 0) {
        s->stats.tx_overflow++;
        return -1;
    }
    s->stats.tx_lines++;
    session_flush_send_buf(s);
    return 0;
}

int session_is_online(const session_t *s)
{
    return s != NULL && ev_fsm_get_state(&s->fsm) == ST_ONLINE;
}

const session_stats_t *session_get_stats(const session_t *s)
{
    return s != NULL ? &s->stats : NULL;
}
