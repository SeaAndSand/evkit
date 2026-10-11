/**
 * @file    ev_loop.h
 * @brief   epoll 事件循环：io / timer / post；每轮派发结束后 poll 已注册的 FSM
 *
 * 线程契约（见 README）：
 * - ev_loop_post / ev_loop_stop：可从任意线程调用（类 GMainLoop idle/invoke）
 * - 其余 API（run/io/timer/register_fsm/deinit 等）：仅 loop 线程
 * - ev_fsm_* 始终仅 loop 线程；跨线程先 post 到 loop，再在回调里 fsm_post
 */
#ifndef EV_LOOP_H
#define EV_LOOP_H

#include "evkit/ev_fsm.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* 常量                                                                       */
/******************************************************************************/

#define EV_LOOP_POST_CAPACITY 64 // post 队列容量
#define EV_LOOP_FSM_MAX       16 // 单 loop 最多注册的 FSM 数
#define EV_LOOP_READ  0x001u     // 对应 EPOLLIN
#define EV_LOOP_WRITE 0x004u     // 对应 EPOLLOUT

/******************************************************************************/
/* 前向声明与回调                                                             */
/******************************************************************************/

typedef struct ev_loop ev_loop_t;
typedef struct ev_io ev_io_t;
typedef struct ev_timer ev_timer_t;

typedef void (*ev_io_fn)(ev_loop_t *loop, ev_io_t *io, uint32_t events);
typedef void (*ev_timer_fn)(ev_loop_t *loop, ev_timer_t *timer);
typedef void (*ev_post_fn)(void *user_data);
typedef void (*ev_post_free_fn)(void *user_data); // 可选：回调结束后或 deinit 丢弃时调用恰好一次；见 ev_loop_post_full

/******************************************************************************/
/* 数据结构                                                                   */
/******************************************************************************/

// 被监控的 fd，由调用者分配并嵌入业务对象，loop 不拥有
struct ev_io {
    int fd;            // 文件描述符（调用者须设 O_NONBLOCK）
    uint32_t events;   // 关心的 epoll 事件
    ev_io_fn callback; // 就绪回调；ev_io_remove 后为 NULL
    void *user_data;   // 业务上下文
};

// 逻辑定时器，挂在 loop 链表上，底层共用一个 timerfd
struct ev_timer {
    ev_loop_t *loop;          // 所属循环；start / start_at 写入，remove 后为 NULL
    ev_timer_fn callback;     // 到期回调
    void *user_data;          // 业务上下文
    uint64_t deadline_ms;     // 下次到期的单调时钟毫秒（与 ev_loop_now_ms_u64 同钟）
    uint32_t interval_ms;     // 周期间隔；0 表示单次，>0 表示周期
    int repeat_from_deadline; // 0: 下次=完成时 now+周期(可漂); 1: 下次=原 deadline+周期(不漂，落后追赶)
    int active;               // 是否已启动
    ev_timer_t *next;         // 链表后继
};

// post 队列的一项
typedef struct {
    ev_post_fn callback;     // 唤醒后在 loop 线程执行的回调
    void *user_data;         // 回传指针
    ev_post_free_fn free_fn; // 可 NULL；见 ev_loop_post_full
} ev_post_item_t;

// post 环形队列（由 post_mu 保护，支持多生产者）
typedef struct {
    ev_post_item_t items[EV_LOOP_POST_CAPACITY]; // 槽位
    uint8_t head;                                // 出队下标
    uint8_t tail;                                // 入队下标
    uint8_t count;                               // 当前个数
} ev_post_queue_t;

// 事件循环：epoll + eventfd + 共用 timerfd + 已注册的 FSM
struct ev_loop {
    int epoll_fd;                    // epoll 实例
    int wakeup_fd;                   // eventfd，用于 post 和 stop
    int timer_fd;                    // 唯一 timerfd
    int running;                     // 是否在 ev_loop_run 中（仅 loop 线程，兼重入守卫）
    atomic_int stop_requested;       // 请求退出；可跨线程置位
    uint32_t post_queue_dropped;     // post_queue 满丢弃累计（在 post_mu 下更新）
    uint32_t post_drain_limit_hits;  // 单次 post drain 触达步数上限的次数
    pthread_mutex_t post_mu;         // 保护 post_queue / post_queue_dropped
    ev_io_t wakeup_io;               // 内部：挂 wakeup_fd
    ev_io_t timer_io;                // 内部：挂 timer_fd
    ev_timer_t *timers;              // 逻辑定时器链表头（仅 loop 线程）
    ev_post_queue_t post_queue;      // post 队列
    ev_fsm_t *fsms[EV_LOOP_FSM_MAX]; // 已注册的 FSM
    int fsm_count;                   // 当前注册数量
};

/******************************************************************************/
/* ev_loop：生命周期与时钟                                                    */
/******************************************************************************/

/**
 * @brief  初始化循环，创建 epoll/eventfd/timerfd 与 post 队列锁
 * @param  loop  事件循环
 * @return 0 成功，-1 失败
 */
int ev_loop_init(ev_loop_t *loop);

/**
 * @brief  关闭内部 fd 并销毁锁；须在无并发 post、且 run 已结束后调用
 * @param  loop  事件循环
 * @return 无
 *
 * 不关闭用户 fd，不摘用户 timer/io；未执行的 post **不调用** callback。
 * - free_fn == NULL（普通 post）：user_data 不释放，调用者须已可丢或已 drain
 * - free_fn != NULL（post_full）：对残留项调用 free_fn（不跑 callback）
 * 见 README「资源所有权」「关机清单」。
 */
void ev_loop_deinit(ev_loop_t *loop);

/**
 * @brief  阻塞运行直到 ev_loop_stop；禁止嵌套调用
 * @param  loop  事件循环
 * @return 0 正常退出，-1 失败（含嵌套调用、epoll_wait 错误）
 *
 * 进入时先 poll 已注册的 FSM（消化 run 前入队），再进入 wait。
 * 进入前若已 stop，立即返回；返回时清零 stop，可再次 run。
 */
int ev_loop_run(ev_loop_t *loop);

/**
 * @brief  请求退出 ev_loop_run
 * @param  loop  事件循环
 * @return 无
 *
 * 线程安全：可从任意线程调用。
 */
void ev_loop_stop(ev_loop_t *loop);

/**
 * @brief  单调时钟毫秒（uint32，约 49 天回绕）
 * @return 毫秒时间戳；clock_gettime 失败时返回 0
 */
uint32_t ev_loop_now_ms(void);

/**
 * @brief  单调时钟毫秒（64 位，供 start_at / deadline 使用）
 * @return 毫秒时间戳；clock_gettime 失败时返回 0
 */
uint64_t ev_loop_now_ms_u64(void);

/******************************************************************************/
/* ev_loop：post 队列（可跨线程）                                             */
/******************************************************************************/

/**
 * @brief  投递回调并唤醒 epoll_wait；回调在 loop 线程执行
 * @param  loop       事件循环
 * @param  callback   回调
 * @param  user_data  回传指针，须活到回调执行完；所有权始终在调用方（库不释放）
 * @return 0 成功，队列满返回 -1（并增加 post_queue_dropped；此时调用方须自行释放 user_data）
 *
 * 线程安全：可从任意线程调用（含 loop 线程）。语义类似 GLib g_idle_add。
 * 等价于 ev_loop_post_full(..., NULL)。库永不 free user_data。
 */
int ev_loop_post(ev_loop_t *loop, ev_post_fn callback, void *user_data);

/**
 * @brief  投递回调，并登记 user_data 的释放函数
 * @param  free_fn  可 NULL（同 ev_loop_post）；非 NULL 时在以下情况恰好调用一次：
 *                  ① callback 返回之后；② deinit 丢弃该项且未调用 callback
 * @return 0 成功，队列满返回 -1（此时不调用 free_fn，调用方自行释放 user_data）
 *
 * 若提供 free_fn，callback 内不要再 free 同一 user_data。
 * ev_bus 的 POST delivery 使用本接口，避免目标 loop deinit 泄漏包装块。
 */
int ev_loop_post_full(ev_loop_t *loop, ev_post_fn callback, void *user_data, ev_post_free_fn free_fn);

/******************************************************************************/
/* ev_loop：FSM 注册                                                          */
/******************************************************************************/

/**
 * @brief  注册 FSM：每轮 epoll 派发结束后自动 ev_fsm_poll
 * @param  loop  事件循环
 * @param  fsm   状态机；同一 fsm 重复注册视为成功
 * @return 0 成功，-1 失败（满 EV_LOOP_FSM_MAX）
 */
int ev_loop_register_fsm(ev_loop_t *loop, ev_fsm_t *fsm);

/**
 * @brief  取消注册 FSM
 * @param  loop  事件循环
 * @param  fsm   已注册的状态机
 * @return 0 成功，-1 未找到
 */
int ev_loop_unregister_fsm(ev_loop_t *loop, ev_fsm_t *fsm);

/******************************************************************************/
/* ev_io                                                                      */
/******************************************************************************/

/**
 * @brief  将 io 加入 epoll（不设置 O_NONBLOCK，由调用者负责）
 * @param  loop  事件循环
 * @param  io    调用者持有的 handle，须填好 fd/events/callback
 * @return 0 成功，-1 失败
 */
int ev_io_add(ev_loop_t *loop, ev_io_t *io);

/**
 * @brief  修改 io 关心的事件
 * @param  loop    事件循环
 * @param  io      已 add 的 handle
 * @param  events  EV_LOOP_READ / EV_LOOP_WRITE
 * @return 0 成功，-1 失败
 */
int ev_io_mod(ev_loop_t *loop, ev_io_t *io, uint32_t events);

/**
 * @brief  从 epoll 删除 io，不关闭 fd，不清 callback
 * @param  loop  事件循环
 * @param  io    已 add 的 handle
 * @return 0 成功，-1 失败
 *
 * 一般关生命周期请用 ev_io_remove；本接口留给仍需保留 callback 的场景。
 */
int ev_io_del(ev_loop_t *loop, ev_io_t *io);

/**
 * @brief  从 epoll 删除并将 callback 置 NULL（推荐的卸下方式）
 * @param  loop  事件循环
 * @param  io    已 add 的 handle
 * @return 0 成功，-1 失败
 *
 * 先 remove；本对象可在当前回调返回前 free。
 * 同批 epoll 事件里其它 io 指针勿 free（events[] 仍可能引用）。
 */
int ev_io_remove(ev_loop_t *loop, ev_io_t *io);

/******************************************************************************/
/* ev_timer                                                                   */
/******************************************************************************/

/**
 * @brief  启动或重启定时器：首次=现在+after_ms；周期下次=完成时 now+周期（长期会漂）
 * @param  loop       事件循环
 * @param  timer      调用者持有的定时器
 * @param  after_ms   首次间隔；0 视为尽快（内部按 1ms）
 * @param  repeat_ms  周期间隔；0 为单次，非 0 为周期
 * @param  callback   到期回调
 * @param  user_data  回传指针
 * @return 0 成功，-1 参数非法
 */
int ev_timer_start(ev_loop_t *loop, ev_timer_t *timer, uint32_t after_ms, uint32_t repeat_ms, ev_timer_fn callback, void *user_data);

/**
 * @brief  按绝对截止时刻启动或重启：周期下次=原 deadline+周期（不漂；落后则追赶到未来）
 * @param  loop         事件循环
 * @param  timer        调用者持有的定时器
 * @param  deadline_ms  首次到期单调时钟毫秒（ev_loop_now_ms_u64 同钟；已过期则尽快触发）
 * @param  repeat_ms    周期间隔；0 为单次，非 0 为周期
 * @param  callback     到期回调
 * @param  user_data    回传指针
 * @return 0 成功，-1 参数非法
 *
 * 相对固定起点 T0：start_at(loop, t, t0 + offset, …)。
 * 整点/凌晨等：业务用日历算出「距下一目标还剩多少 ms」，再
 * start_at(loop, t, ev_loop_now_ms_u64() + remain_ms, period, …)。
 */
int ev_timer_start_at(ev_loop_t *loop, ev_timer_t *timer, uint64_t deadline_ms, uint32_t repeat_ms, ev_timer_fn callback, void *user_data);

/**
 * @brief  停止定时器，仍留在链表上，可再次 start / start_at
 * @param  timer  已 start 的定时器（使用 timer->loop）
 * @return 0 成功，-1 参数非法或未 start
 */
int ev_timer_stop(ev_timer_t *timer);

/**
 * @brief  停止并从链表摘下（不释放 timer 内存）；对象可释放或复用前须调用
 * @param  timer  定时器（使用 timer->loop；未 start 过则返回 -1）
 * @return 0 成功，-1 参数非法
 *
 * 先 remove；本对象可在当前回调返回前 free（派发按次重扫链表，可 remove 兄弟）。
 * 与 io 同原则：摘链后再 free。
 */
int ev_timer_remove(ev_timer_t *timer);

#ifdef __cplusplus
}
#endif

#endif
