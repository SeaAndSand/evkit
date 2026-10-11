/**
 * @file    session.h
 * @brief   TCP 会话：连接态机 + 非阻塞读写 + 自动重连
 *
 * 对外只暴露：init/start/stop、send_line、统计计数。
 * 收到完整一行后回调 on_line；断线回调 on_offline（用于取消异步命令）。
 */
#ifndef TCP_CMD_SESSION_H
#define TCP_CMD_SESSION_H

#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"
#include "net_buf.h"
#include "proto.h"

#include <stdint.h>

struct session;

typedef void (*session_line_fn)(struct session *s, const char *line, void *user_data);
typedef void (*session_offline_fn)(struct session *s, void *user_data);
typedef void (*session_online_fn)(struct session *s, void *user_data);

typedef struct {
    uint32_t connect_ok;
    uint32_t connect_fail;
    uint32_t disconnect;
    uint32_t reconnect;
    uint32_t rx_lines;
    uint32_t tx_lines;
    uint32_t line_too_long;
    uint32_t rx_overflow;
    uint32_t tx_overflow;
    uint32_t write_err;
} session_stats_t;

typedef struct session {
    ev_loop_t *loop;
    ev_fsm_t fsm;
    ev_io_t io;
    ev_timer_t connect_timer;
    ev_timer_t retry_timer;
    char host[128];
    uint16_t port;
    int fd;
    int write_pending; /* 发送缓冲未发完：需要监听 EPOLLOUT */
    net_buf_t recv_buf;
    net_buf_t send_buf;
    session_line_fn on_line;
    session_online_fn on_online;
    session_offline_fn on_offline;
    void *user_data;
    session_stats_t stats;
    int stop_requested; /* stop 后不再重连 */
} session_t;

int session_init(session_t *s, ev_loop_t *loop, const char *host, uint16_t port, session_line_fn on_line, session_online_fn on_online, session_offline_fn on_offline, void *user_data);
void session_deinit(session_t *s);
void session_start(session_t *s);
void session_stop(session_t *s);
int session_send_line(session_t *s, const char *line);
int session_is_online(const session_t *s);
const session_stats_t *session_get_stats(const session_t *s);

#endif
