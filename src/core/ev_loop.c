/**
 * @file    ev_loop.c
 * @brief   epoll 事件循环实现
 */
#define _GNU_SOURCE
#include "evkit/ev_loop.h"

#include <errno.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define EV_LOOP_POST_DRAIN_MAX (EV_LOOP_POST_CAPACITY * 4u)

/******************************************************************************/
/* 时钟（公开） */
/******************************************************************************/
/**
 * @brief  单调时钟毫秒
 * @return 毫秒时间戳；失败返回 0
 */
uint32_t ev_loop_now_ms(void)
{
    return (uint32_t)ev_loop_now_ms_u64();
}

/**
 * @brief  单调时钟毫秒（64 位，供 start_at / deadline 使用）
 * @return 毫秒时间戳；失败返回 0
 */
uint64_t ev_loop_now_ms_u64(void)
{
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000u);
}

/******************************************************************************/
/* 内部：post 队列 */
/******************************************************************************/
/**
 * @brief  post 入队
 * @return 0 成功，满返回 -1
 */
static int post_queue_push(ev_post_queue_t *queue, ev_post_fn callback, void *user_data, ev_post_free_fn free_fn)
{
    if (queue->count >= EV_LOOP_POST_CAPACITY) {
        return -1;
    }
    queue->items[queue->tail].callback = callback;
    queue->items[queue->tail].user_data = user_data;
    queue->items[queue->tail].free_fn = free_fn;
    queue->tail = (uint8_t)((queue->tail + 1u) % EV_LOOP_POST_CAPACITY);
    queue->count++;
    return 0;
}

/**
 * @brief  post出队
 * @param  queue  post 队列
 * @param  out    输出项
 * @return 1 有数据，0 空
 */
static int post_queue_pop(ev_post_queue_t *queue, ev_post_item_t *out)
{
    if (queue->count == 0) {
        return 0;
    }
    *out = queue->items[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) % EV_LOOP_POST_CAPACITY);
    queue->count--;
    return 1;
}

/**
 * @brief  排空post 队列并依次回调
 * @param  loop  事件循环
 * @return 无
 *
 * 在锁内出队、锁外执行回调，避免回调里再 post 死锁。
 * 单次最多 EV_LOOP_POST_DRAIN_MAX 步。
 */
static void post_queue_drain(ev_loop_t *loop)
{
    uint32_t steps = 0;
    for (;;) {
        ev_post_item_t item = {0};
        int has = 0;

        pthread_mutex_lock(&loop->post_mu);
        has = post_queue_pop(&loop->post_queue, &item);
        pthread_mutex_unlock(&loop->post_mu);

        if (!has) {
            break;
        }
        if (item.callback != NULL) {
            item.callback(item.user_data);
        }
        if (item.free_fn != NULL) {
            item.free_fn(item.user_data);
        }
        steps++;
        if (steps >= EV_LOOP_POST_DRAIN_MAX) {
            loop->post_drain_limit_hits++;
            break;
        }
    }
}

/** deinit：不跑 callback，仅对带 free_fn 的残留项释放 */
static void post_queue_discard_all(ev_loop_t *loop)
{
    for (;;) {
        ev_post_item_t item = {0};
        int has = 0;

        pthread_mutex_lock(&loop->post_mu);
        has = post_queue_pop(&loop->post_queue, &item);
        pthread_mutex_unlock(&loop->post_mu);

        if (!has) {
            break;
        }
        if (item.free_fn != NULL) {
            item.free_fn(item.user_data);
        }
    }
}

/******************************************************************************/
/* 内部：wakeup / timerfd */
/******************************************************************************/
/**
 * @brief  向 eventfd 写入 1，唤醒 epoll_wait
 * @param  fd  wakeup_fd
 * @return 无
 */
static void wakeup_write(int fd)
{
    uint64_t one = 1;
    for (;;) {
        ssize_t n = write(fd, &one, sizeof(one));
        if (n == (ssize_t)sizeof(one)) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        // EAGAIN：计数将饱和，仍视为已唤醒；其它错误无法再补
        return;
    }
}

/**
 * @brief  按最近到期的逻辑定时器重装 timerfd
 * @param  loop  事件循环
 * @return 无
 */
static void update_timer_fd(ev_loop_t *loop)
{
    uint64_t nearest = UINT64_MAX;
    for (ev_timer_t *timer = loop->timers; timer != NULL; timer = timer->next) {
        if (!timer->active) {
            continue;
        }
        if (timer->deadline_ms < nearest) {
            nearest = timer->deadline_ms;
        }
    }

    struct itimerspec spec = {0};
    if (nearest == UINT64_MAX) {
        (void)timerfd_settime(loop->timer_fd, 0, &spec, NULL);
        return;
    }

    uint64_t now = ev_loop_now_ms_u64();
    uint64_t delay = (nearest > now) ? (nearest - now) : 1;
    spec.it_value.tv_sec = (time_t)(delay / 1000u);
    spec.it_value.tv_nsec = (long)((delay % 1000u) * 1000000u);
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
        spec.it_value.tv_nsec = 1000000L;
    }
    (void)timerfd_settime(loop->timer_fd, 0, &spec, NULL);
}

/**
 * @brief  判断定时器是否已挂在 loop 链表上
 * @param  loop   事件循环
 * @param  timer  定时器
 * @return 1 已在链表，0 不在
 */
static int timer_on_list(ev_loop_t *loop, ev_timer_t *timer)
{
    for (ev_timer_t *p = loop->timers; p != NULL; p = p->next) {
        if (p == timer) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief  从 loop 定时器链表摘下节点
 * @param  loop   事件循环
 * @param  timer  定时器
 * @return 无
 */
static void timer_unlink(ev_loop_t *loop, ev_timer_t *timer)
{
    ev_timer_t **pp = &loop->timers;
    while (*pp != NULL) {
        if (*pp == timer) {
            *pp = timer->next;
            break;
        }
        pp = &(*pp)->next;
    }
    timer->next = NULL;
}

/**
 * @brief  eventfd 可读：清计数并执行post 队列
 * @param  loop    事件循环
 * @param  io      内部 wakeup_io
 * @param  events  epoll 事件
 * @return 无
 */
static void on_wakeup_io(ev_loop_t *loop, ev_io_t *io, uint32_t events)
{
    (void)io;
    (void)events;
    uint64_t value = 0;
    for (;;) {
        ssize_t n = read(loop->wakeup_fd, &value, sizeof(value));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    post_queue_drain(loop);
}

/**
 * @brief  timerfd 可读：触发已到期的逻辑定时器
 * @param  loop    事件循环
 * @param  io      内部 timer_io
 * @param  events  epoll 事件
 * @return 无
 *
 * 每次只触发一个到期 timer，回调返回后从链表头重扫。
 * 这样回调里 destroy/free 其它定时器不会 UAF（不能沿用旧的 next 指针）。
 */
static void on_timer_io(ev_loop_t *loop, ev_io_t *io, uint32_t events)
{
    (void)io;
    (void)events;
    uint64_t expired = 0;
    for (;;) {
        ssize_t n = read(loop->timer_fd, &expired, sizeof(expired));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }

    uint64_t now = ev_loop_now_ms_u64();
    for (;;) {
        ev_timer_t *due = NULL;
        for (ev_timer_t *timer = loop->timers; timer != NULL; timer = timer->next) {
            if (timer->active && timer->deadline_ms <= now) {
                due = timer;
                break;
            }
        }
        if (due == NULL) {
            break;
        }

        ev_timer_fn callback = due->callback;
        if (due->interval_ms > 0) {
            if (due->repeat_from_deadline) {
                /* 相对原排程累加；落后多个周期则追赶到未来，避免连响风暴 */
                due->deadline_ms += due->interval_ms;
                while (due->deadline_ms <= now) {
                    due->deadline_ms += due->interval_ms;
                }
            } else {
                /* 相对本次完成时刻；长期运行周期会漂 */
                due->deadline_ms = now + due->interval_ms;
            }
        } else {
            due->active = 0;
        }
        if (callback != NULL) {
            callback(loop, due);
        }
    }
    update_timer_fd(loop);
}

/******************************************************************************/
/* ev_loop：生命周期 */
/******************************************************************************/
/**
 * @brief  初始化循环，创建 epoll/eventfd/timerfd
 * @param  loop  事件循环
 * @return 0 成功，-1 失败
 */
int ev_loop_init(ev_loop_t *loop)
{
    if (loop == NULL) {
        return -1;
    }
    memset(loop, 0, sizeof(*loop));
    loop->epoll_fd = -1;
    loop->wakeup_fd = -1;
    loop->timer_fd = -1;
    atomic_init(&loop->stop_requested, 0);
    if (pthread_mutex_init(&loop->post_mu, NULL) != 0) {
        return -1;
    }

    loop->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epoll_fd < 0) {
        pthread_mutex_destroy(&loop->post_mu);
        return -1;
    }
    loop->wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wakeup_fd < 0) {
        ev_loop_deinit(loop);
        return -1;
    }
    loop->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (loop->timer_fd < 0) {
        ev_loop_deinit(loop);
        return -1;
    }

    loop->wakeup_io = (ev_io_t){ .fd = loop->wakeup_fd, .events = EV_LOOP_READ, .callback = on_wakeup_io, .user_data = loop };
    if (ev_io_add(loop, &loop->wakeup_io) != 0) {
        ev_loop_deinit(loop);
        return -1;
    }
    loop->timer_io = (ev_io_t){ .fd = loop->timer_fd, .events = EV_LOOP_READ, .callback = on_timer_io, .user_data = loop };
    if (ev_io_add(loop, &loop->timer_io) != 0) {
        ev_loop_deinit(loop);
        return -1;
    }
    return 0;
}

/**
 * @brief  关闭内部 fd 并销毁锁
 * @param  loop  事件循环
 * @return 无
 */
void ev_loop_deinit(ev_loop_t *loop)
{
    if (loop == NULL) {
        return;
    }
    loop->timers = NULL;
    post_queue_discard_all(loop);
    if (loop->timer_fd >= 0) {
        close(loop->timer_fd);
        loop->timer_fd = -1;
    }
    if (loop->wakeup_fd >= 0) {
        close(loop->wakeup_fd);
        loop->wakeup_fd = -1;
    }
    if (loop->epoll_fd >= 0) {
        close(loop->epoll_fd);
        loop->epoll_fd = -1;
    }
    pthread_mutex_destroy(&loop->post_mu);
}

/******************************************************************************/
/* ev_io */
/******************************************************************************/
/**
 * @brief  将 io 加入 epoll
 * @param  loop  事件循环
 * @param  io    调用者持有的 handle
 * @return 0 成功，-1 失败
 */
int ev_io_add(ev_loop_t *loop, ev_io_t *io)
{
    if (loop == NULL || io == NULL || io->fd < 0) {
        return -1;
    }
    struct epoll_event ev = { .events = io->events, .data.ptr = io };
    return epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, io->fd, &ev);
}

/**
 * @brief  修改 io 关心的事件
 * @param  loop    事件循环
 * @param  io      已 add 的 handle
 * @param  events  EV_LOOP_READ / EV_LOOP_WRITE
 * @return 0 成功，-1 失败
 */
int ev_io_mod(ev_loop_t *loop, ev_io_t *io, uint32_t events)
{
    if (loop == NULL || io == NULL || io->fd < 0) {
        return -1;
    }
    io->events = events;
    struct epoll_event ev = { .events = events, .data.ptr = io };
    return epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, io->fd, &ev);
}

/**
 * @brief  从 epoll 删除 io，不关闭 fd
 * @param  loop  事件循环
 * @param  io    已 add 的 handle
 * @return 0 成功，-1 失败
 */
int ev_io_del(ev_loop_t *loop, ev_io_t *io)
{
    if (loop == NULL || io == NULL || io->fd < 0) {
        return -1;
    }
    return epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, io->fd, NULL);
}

/**
 * @brief  从 epoll 删除并将 callback 置 NULL
 * @param  loop  事件循环
 * @param  io    handle
 * @return 0 成功，-1 失败
 */
int ev_io_remove(ev_loop_t *loop, ev_io_t *io)
{
    if (loop == NULL || io == NULL) {
        return -1;
    }
    int rc = 0;
    if (io->fd >= 0) {
        rc = ev_io_del(loop, io);
    }
    io->callback = NULL;
    return rc;
}

/******************************************************************************/
/* ev_timer */
/******************************************************************************/
/**
 * @brief  挂上/更新定时器公共逻辑
 */
static int timer_setup(ev_loop_t *loop, ev_timer_t *timer, uint64_t deadline_ms, uint32_t repeat_ms, int repeat_from_deadline, ev_timer_fn callback, void *user_data)
{
    if (loop == NULL || timer == NULL || callback == NULL) {
        return -1;
    }
    timer->loop = loop;
    timer->callback = callback;
    timer->user_data = user_data;
    timer->interval_ms = repeat_ms;
    timer->repeat_from_deadline = repeat_from_deadline ? 1 : 0;
    timer->deadline_ms = deadline_ms;
    timer->active = 1;
    if (!timer_on_list(loop, timer)) {
        timer->next = loop->timers;
        loop->timers = timer;
    }
    update_timer_fd(loop);
    return 0;
}

/**
 * @brief  启动或重启：首次=现在+after；周期下次=完成时 now+周期（可漂）
 */
int ev_timer_start(ev_loop_t *loop, ev_timer_t *timer, uint32_t after_ms, uint32_t repeat_ms, ev_timer_fn callback, void *user_data)
{
    uint32_t first = (after_ms == 0) ? 1u : after_ms;
    return timer_setup(loop, timer, ev_loop_now_ms_u64() + first, repeat_ms, 0, callback, user_data);
}

/**
 * @brief  按绝对截止时刻启动或重启；周期下次=原 deadline+周期（不漂）
 */
int ev_timer_start_at(ev_loop_t *loop, ev_timer_t *timer, uint64_t deadline_ms, uint32_t repeat_ms, ev_timer_fn callback, void *user_data)
{
    return timer_setup(loop, timer, deadline_ms, repeat_ms, 1, callback, user_data);
}

/**
 * @brief  停止定时器，对象仍可再次 start
 * @param  timer  已 start 的定时器（使用 timer->loop）
 * @return 0 成功，-1 参数非法或未 start
 */
int ev_timer_stop(ev_timer_t *timer)
{
    if (timer == NULL || timer->loop == NULL) {
        return -1;
    }
    timer->active = 0;
    update_timer_fd(timer->loop);
    return 0;
}

/**
 * @brief  停止并从链表摘下；对象可释放或复用前须调用
 * @param  timer  定时器（使用 timer->loop）
 * @return 0 成功，-1 参数非法或未 start
 */
int ev_timer_remove(ev_timer_t *timer)
{
    if (timer == NULL || timer->loop == NULL) {
        return -1;
    }
    ev_loop_t *loop = timer->loop;
    timer->active = 0;
    timer_unlink(loop, timer);
    timer->loop = NULL;
    timer->callback = NULL;
    timer->user_data = NULL;
    update_timer_fd(loop);
    return 0;
}

/******************************************************************************/
/* ev_loop：post 队列 */
/******************************************************************************/
/**
 * @brief  投递 post 并唤醒 epoll_wait（可跨线程）
 */
int ev_loop_post_full(ev_loop_t *loop, ev_post_fn callback, void *user_data, ev_post_free_fn free_fn)
{
    if (loop == NULL || callback == NULL) {
        return -1;
    }

    pthread_mutex_lock(&loop->post_mu);
    int rc = post_queue_push(&loop->post_queue, callback, user_data, free_fn);
    if (rc != 0) {
        loop->post_queue_dropped++;
    }
    pthread_mutex_unlock(&loop->post_mu);

    if (rc != 0) {
        return -1;
    }
    if (loop->wakeup_fd >= 0) {
        wakeup_write(loop->wakeup_fd);
    }
    return 0;
}

int ev_loop_post(ev_loop_t *loop, ev_post_fn callback, void *user_data)
{
    return ev_loop_post_full(loop, callback, user_data, NULL);
}

/**
 * @brief  请求退出 ev_loop_run（可跨线程）
 * @param  loop  事件循环
 * @return 无
 */
void ev_loop_stop(ev_loop_t *loop)
{
    if (loop == NULL) {
        return;
    }
    atomic_store_explicit(&loop->stop_requested, 1, memory_order_release);
    if (loop->wakeup_fd >= 0) {
        wakeup_write(loop->wakeup_fd);
    }
}

/******************************************************************************/
/* ev_loop：FSM 注册 */
/******************************************************************************/
/**
 * @brief  注册 FSM：每轮派发结束后自动 poll；重复注册同一 fsm 视为成功
 * @param  loop  事件循环
 * @param  fsm   状态机
 * @return 0 成功，-1 失败（满）
 */
int ev_loop_register_fsm(ev_loop_t *loop, ev_fsm_t *fsm)
{
    if (loop == NULL || fsm == NULL) {
        return -1;
    }
    for (int i = 0; i < loop->fsm_count; i++) {
        if (loop->fsms[i] == fsm) {
            return 0;
        }
    }
    if (loop->fsm_count >= EV_LOOP_FSM_MAX) {
        return -1;
    }
    loop->fsms[loop->fsm_count++] = fsm;
    return 0;
}

/**
 * @brief  取消注册 FSM
 * @param  loop  事件循环
 * @param  fsm   已注册的状态机
 * @return 0 成功，-1 未找到
 */
int ev_loop_unregister_fsm(ev_loop_t *loop, ev_fsm_t *fsm)
{
    if (loop == NULL || fsm == NULL) {
        return -1;
    }
    for (int i = 0; i < loop->fsm_count; i++) {
        if (loop->fsms[i] != fsm) {
            continue;
        }
        for (int j = i + 1; j < loop->fsm_count; j++) {
            loop->fsms[j - 1] = loop->fsms[j];
        }
        loop->fsm_count--;
        loop->fsms[loop->fsm_count] = NULL;
        return 0;
    }
    return -1;
}

/******************************************************************************/
/* ev_loop：run */
/******************************************************************************/
/**
 * @brief  对本轮开始时已注册的 FSM 各做一次 poll
 * @param  loop  事件循环
 * @return 无
 *
 * 先快照指针再 poll：回调里 unregister 压缩数组时不会跳过后续 FSM。
 * 本轮中途被注销的跳过；本轮新 register 的留到下一轮。
 */
static void poll_registered_fsms(ev_loop_t *loop)
{
    ev_fsm_t *snap[EV_LOOP_FSM_MAX];
    int n = loop->fsm_count;
    if (n > EV_LOOP_FSM_MAX) {
        n = EV_LOOP_FSM_MAX;
    }
    for (int i = 0; i < n; i++) {
        snap[i] = loop->fsms[i];
    }
    for (int i = 0; i < n; i++) {
        int still = 0;
        for (int j = 0; j < loop->fsm_count; j++) {
            if (loop->fsms[j] == snap[i]) {
                still = 1;
                break;
            }
        }
        if (still) {
            ev_fsm_poll(snap[i]);
        }
    }
}

/**
 * @brief  阻塞运行直到 ev_loop_stop；禁止嵌套调用
 * @param  loop  事件循环
 * @return 0 正常退出，-1 失败（含嵌套调用、epoll_wait 错误）
 *
 * 进入时先 poll 已注册的 FSM；每轮 io/timer/post 派发后再 poll。
 * 返回时清零 stop，可再次 run。
 */
int ev_loop_run(ev_loop_t *loop)
{
    if (loop == NULL) {
        return -1;
    }
    if (loop->running) {
        return -1;
    }

    loop->running = 1;

    // run 前已 post 的事件：先 poll 再进入 wait，避免无 fd 就绪时饿死
    poll_registered_fsms(loop);

    while (!atomic_load_explicit(&loop->stop_requested, memory_order_acquire)) {
        struct epoll_event events[16] = {0};
        int n = epoll_wait(loop->epoll_fd, events, 16, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            loop->running = 0;
            atomic_store_explicit(&loop->stop_requested, 0, memory_order_relaxed);
            return -1;
        }
        for (int i = 0; i < n; i++) {
            ev_io_t *io = events[i].data.ptr;
            if (io != NULL && io->callback != NULL) {
                io->callback(loop, io, events[i].events);
            }
        }
        poll_registered_fsms(loop);
    }

    loop->running = 0;
    atomic_store_explicit(&loop->stop_requested, 0, memory_order_relaxed);
    return 0;
}
