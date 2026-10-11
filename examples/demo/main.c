/**
 * @file    main.c
 * @brief   官方写法模板：回调只 post；ev_loop_register_fsm 后由 loop 回合末 poll
 */
#include "evkit/ev_fsm.h"
#include "evkit/ev_loop.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum {
    ST_IDLE = 0,
    ST_CONNECTING,
    ST_ONLINE,
    ST_WAIT,
    ST_COUNT
};

enum {
    EV_START = 1,
    EV_CONNECT_OK,
    EV_LOST,
    EV_RETRY,
    EV_RX,
    EV_STOP
};

typedef struct {
    ev_loop_t  *loop;            // 事件循环
    ev_fsm_t   fsm;              // 事件状态机
    ev_timer_t connect_timer;    // 连接定时器
    ev_timer_t retry_timer;      // 重试定时器
    ev_timer_t heartbeat_timer;  // 心跳定时器
    ev_io_t    rx_io;            // 读写事件
    int        pipe_read;        // 管道读描述符
    int        pipe_write;       // 管道写描述符
    int        rx_count;         // 读取计数
    char       rx_scratch[16];   // 读取缓冲区
} session_t;

static void demo_post_event(session_t *session, int event, void *arg)
{
    if (ev_fsm_post(&session->fsm, event, arg) != 0) {
        fprintf(stderr, "fsm post overflow event=%d dropped=%u\n", event, session->fsm.post_dropped);
    }
}

static void timers_deinit(session_t *session)
{
    (void)ev_timer_remove(&session->connect_timer);
    (void)ev_timer_remove(&session->retry_timer);
    (void)ev_timer_remove(&session->heartbeat_timer);
}

static void on_connect_timer(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *session = timer->user_data;
    printf("  [timer] connect ok\n");
    demo_post_event(session, EV_CONNECT_OK, NULL);
}

static void on_retry_timer(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *session = timer->user_data;
    printf("  [timer] retry\n");
    demo_post_event(session, EV_RETRY, NULL);
}

static void on_heartbeat(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *session = timer->user_data;
    printf("  [timer] heartbeat state=%s\n", ev_fsm_state_name(&session->fsm));
}

static void enter_connecting(void *user_data, int state)
{
    (void)state;
    session_t *session = user_data;
    printf("  [enter] CONNECTING\n");
    ev_timer_start(session->loop, &session->connect_timer, 30, 0, on_connect_timer, session);
}

static void exit_connecting(void *user_data, int state)
{
    (void)state;
    session_t *session = user_data;
    (void)ev_timer_stop(&session->connect_timer);
}

static void enter_online(void *user_data, int state)
{
    (void)state;
    session_t *session = user_data;
    printf("  [enter] ONLINE\n");
    ev_timer_start(session->loop, &session->heartbeat_timer, 40, 40, on_heartbeat, session);
}

static void exit_online(void *user_data, int state)
{
    (void)state;
    session_t *session = user_data;
    printf("  [exit] ONLINE\n");
    (void)ev_timer_stop(&session->heartbeat_timer);
}

static void enter_wait(void *user_data, int state)
{
    (void)state;
    session_t *session = user_data;
    printf("  [enter] WAIT\n");
    ev_timer_start(session->loop, &session->retry_timer, 40, 0, on_retry_timer, session);
}

static void exit_wait(void *user_data, int state)
{
    (void)state;
    session_t *session = user_data;
    (void)ev_timer_stop(&session->retry_timer);
}

static void action_log_rx(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    session_t *session = user_data;
    session->rx_count++;
    printf("  [action] rx '%s' count=%d\n", session->rx_scratch, session->rx_count);
}

static void action_stop(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    session_t *session = user_data;
    timers_deinit(session);
    printf("  [action] STOP → IDLE\n");
}

static void action_fault(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    session_t *session = user_data;
    timers_deinit(session);
    printf("  [action] FAULT → IDLE (poll_limit_hits=%u)\n", session->fsm.poll_limit_hits);
}

static void enter_idle(void *user_data, int state)
{
    (void)user_data;
    (void)state;
    printf("  [enter] IDLE\n");
}

static void exit_idle(void *user_data, int state)
{
    (void)user_data;
    (void)state;
    printf("  [exit] IDLE\n");
}

static const ev_fsm_transition_t k_edges[] = {
    { .from_state = ST_IDLE,       .event = EV_START,       .to_state = ST_CONNECTING,  .action = NULL },
    { .from_state = ST_CONNECTING, .event = EV_CONNECT_OK,  .to_state = ST_ONLINE,      .action = NULL },
    { .from_state = ST_ONLINE,     .event = EV_RX,          .to_state = EV_FSM_STAY,    .action = action_log_rx },
    { .from_state = ST_ONLINE,     .event = EV_LOST,        .to_state = ST_WAIT,        .action = NULL },
    { .from_state = ST_WAIT,       .event = EV_RETRY,       .to_state = ST_CONNECTING,  .action = NULL },
    { .from_state = EV_FSM_ANY,    .event = EV_STOP,        .to_state = ST_IDLE,        .action = action_stop },
    { .from_state = EV_FSM_ANY,    .event = EV_FSM_FAULT,   .to_state = ST_IDLE,        .action = action_fault },
};

static const ev_fsm_state_t k_states[ST_COUNT] = {
    [ST_IDLE]       = { .name = "IDLE",       .on_enter = enter_idle,       .on_exit = exit_idle },
    [ST_CONNECTING] = { .name = "CONNECTING", .on_enter = enter_connecting, .on_exit = exit_connecting },
    [ST_ONLINE]     = { .name = "ONLINE",     .on_enter = enter_online,     .on_exit = exit_online },
    [ST_WAIT]       = { .name = "WAIT",       .on_enter = enter_wait,       .on_exit = exit_wait },
};

static void on_pipe_readable(ev_loop_t *loop, ev_io_t *io, uint32_t events)
{
    (void)loop;
    (void)events;
    session_t *session = io->user_data;
    ssize_t n = read(session->pipe_read, session->rx_scratch, sizeof(session->rx_scratch) - 1);
    if (n <= 0) {
        return;
    }
    session->rx_scratch[n] = '\0';
    demo_post_event(session, EV_RX, NULL);
}

static void on_script_start(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *session = timer->user_data;
    printf("[script] START\n");
    demo_post_event(session, EV_START, NULL);
}

static void on_script_rx(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *session = timer->user_data;
    printf("[script] write pipe\n");
    (void)write(session->pipe_write, "cell", 4);
}

static void on_script_lost(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *session = timer->user_data;
    printf("[script] LOST\n");
    demo_post_event(session, EV_LOST, NULL);
}

static void on_script_stop(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    session_t *session = timer->user_data;
    printf("[script] STOP\n");
    demo_post_event(session, EV_STOP, NULL);
    ev_loop_stop(session->loop);
}

typedef struct {
    uint32_t after_ms;
    ev_timer_fn callback;
} script_step_t;

int main(void)
{
    // 初始化事件循环
    ev_loop_t loop = {0};
    if (ev_loop_init(&loop) != 0) {
        fprintf(stderr, "ev_loop_init failed\n");
        return 1;
    }

    // 初始化会话
    session_t session = {0};
    session.loop = &loop;

    // 初始化管道
    int fds[2] = {0};
    if (pipe(fds) != 0) {
        ev_loop_deinit(&loop);
        return 1;
    }
    // 将管道读写描述符注册到会话
    session.pipe_read = fds[0];
    session.pipe_write = fds[1];
    // 设置管道为非阻塞
    (void)fcntl(session.pipe_read, F_SETFL, O_NONBLOCK);
    (void)fcntl(session.pipe_write, F_SETFL, O_NONBLOCK);

    session.rx_io = (ev_io_t){ .fd = session.pipe_read, .events = EV_LOOP_READ, .callback = on_pipe_readable, .user_data = &session };
    // 注册RXIO到epoll
    (void)ev_io_add(&loop, &session.rx_io);

    // 初始化事件状态机
    if (ev_fsm_init(&session.fsm, ST_IDLE, k_edges, sizeof(k_edges) / sizeof(k_edges[0]), k_states, ST_COUNT, &session, NULL) != 0) {
        fprintf(stderr, "ev_fsm_init failed (need FAULT edge)\n");
        return 1;
    }
    // 注册事件状态机到epoll
    if (ev_loop_register_fsm(&loop, &session.fsm) != 0) {
        fprintf(stderr, "ev_loop_register_fsm failed\n");
        return 1;
    }

    // 脚本：相对同一起点 t0 的绝对截止（start_at）
    static const script_step_t k_script[] = {
        { .after_ms = 10,  .callback = on_script_start},
        { .after_ms = 90,  .callback = on_script_rx   },
        { .after_ms = 140, .callback = on_script_lost },
        { .after_ms = 260, .callback = on_script_stop },
    };
    ev_timer_t script_timers[sizeof(k_script) / sizeof(k_script[0])] = {0};
    uint64_t t0 = ev_loop_now_ms_u64();
    for (size_t i = 0; i < sizeof(k_script) / sizeof(k_script[0]); i++) {
        (void)ev_timer_start_at(&loop, &script_timers[i], t0 + k_script[i].after_ms, 0, k_script[i].callback, &session);
    }

    printf("======== evkit demo: register_fsm + post/poll ========\n");
    // 运行事件循环
    (void)ev_loop_run(&loop);
    printf("final state=%s rx_count=%d post_queue_dropped=%u fsm_dropped=%u\n", ev_fsm_state_name(&session.fsm), session.rx_count, loop.post_queue_dropped, session.fsm.post_dropped);

    for (size_t i = 0; i < sizeof(script_timers) / sizeof(script_timers[0]); i++) {
        // 销毁脚本定时器
        (void)ev_timer_remove(&script_timers[i]);
    }
    // 销毁会话定时器
    timers_deinit(&session);
    // 注销RXIO从epoll
    (void)ev_io_remove(&loop, &session.rx_io);
    // 关闭管道读写
    close(session.pipe_read);
    close(session.pipe_write);
    (void)ev_loop_unregister_fsm(&loop, &session.fsm);
    ev_fsm_deinit(&session.fsm);
    ev_loop_deinit(&loop);
    return 0;
}
