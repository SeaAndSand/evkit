/**
 * @file    ev_fsm.h
 * @brief   队列边表状态机：回调里 post，loop 注册后每轮统一 poll
 *
 * 契约：仅所属 loop 线程调用；跨线程先 ev_loop_post 再 post。
 * 边表必须含 from_state==EV_FSM_ANY && event==EV_FSM_FAULT 的全局收敛边。
 * arg：入队后由库经 free_arg 释放（溢出 / poll 处理完 / FAULT 清队 / ev_fsm_deinit）；
 *      action 不可接管。堆 arg 必须提供非 NULL 的 free_arg。
 * 与 ev_loop_post(user_data) 相反：loop 侧 user_data 始终由调用者释放。
 * 禁止在 poll / action / enter / exit 内调用 ev_fsm_deinit（会直接返回且不清队）。
 */
#ifndef EV_FSM_H
#define EV_FSM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* 常量                                                                       */
/******************************************************************************/

#define EV_FSM_QUEUE_CAPACITY 16  // 事件队列容量
#define EV_FSM_POLL_MAX_STEPS 256 // 单次 poll 最多迁移步数，防止 enter/post 成环
#define EV_FSM_ANY   (-1)         // from_state / event：通配；精确边优先于通配
#define EV_FSM_STAY  (-2)         // to_state：保持状态，不触发 on_exit/on_enter
#define EV_FSM_FAULT (-3)         // 步数上限触发的收敛事件；边表须含 ANY+FAULT 边

/******************************************************************************/
/* 回调类型                                                                   */
/******************************************************************************/

typedef void (*ev_fsm_action_fn)(void *user_data, int event, void *arg);
typedef void (*ev_fsm_hook_fn)(void *user_data, int state);
typedef void (*ev_fsm_free_arg_fn)(void *arg); // 库在出队处理完、丢弃或 deinit 时调用；可 NULL

/******************************************************************************/
/* 数据结构                                                                   */
/******************************************************************************/

// 一条迁移边
typedef struct {
    int from_state;          // 当前状态，可为 EV_FSM_ANY
    int event;               // 事件，可为 EV_FSM_ANY / EV_FSM_FAULT
    int to_state;            // 下一状态，可为 EV_FSM_STAY
    ev_fsm_action_fn action; // 迁移动作，可 NULL
} ev_fsm_transition_t;

// 状态描述，按下标对应状态编号
typedef struct {
    const char *name;        // 状态名，仅用于日志
    ev_fsm_hook_fn on_enter; // 进入时调用，可 NULL
    ev_fsm_hook_fn on_exit;  // 离开时调用，可 NULL
} ev_fsm_state_t;

// 队列中的一项
typedef struct {
    int event; // 事件号
    void *arg; // 可选载荷
} ev_fsm_item_t;

// 事件环形队列
typedef struct {
    ev_fsm_item_t items[EV_FSM_QUEUE_CAPACITY]; // 槽位
    uint8_t head;                              // 出队下标
    uint8_t tail;                              // 入队下标
    uint8_t count;                             // 当前个数
} ev_fsm_queue_t;

// 状态机实例
typedef struct ev_fsm {
    int state;                              // 当前状态
    ev_fsm_queue_t queue;                   // 待处理事件
    const ev_fsm_transition_t *transitions; // 边表，可放 Flash
    size_t transition_count;                // 边数量
    const ev_fsm_state_t *states;           // 可选状态表
    size_t state_count;                     // states 元素数
    void *user_data;                        // 传给 action/hook
    ev_fsm_free_arg_fn free_arg;            // 释放 arg 的回调，可 NULL
    int is_polling;                         // 防重入
    uint32_t post_dropped;                  // 队列满丢弃累计
    uint32_t poll_limit_hits;               // 单次 poll 触达步数上限的次数
} ev_fsm_t;

/******************************************************************************/
/* 生命周期                                                                   */
/******************************************************************************/

/**
 * @brief  初始化状态机，并对初始状态调用 on_enter
 * @param  fsm                状态机
 * @param  initial_state      初始状态编号
 * @param  transitions        边表，可放 const/Flash；须含 ANY+EV_FSM_FAULT 边
 * @param  transition_count   边数量
 * @param  states             可选，按下标对应状态；无 hook 可传 NULL
 * @param  state_count        states 元素数
 * @param  user_data          传给 action/hook 的上下文
 * @param  free_arg           释放 arg 的回调；可 NULL（见文件头契约）
 * @return 0 成功，-1 失败（含缺全局 FAULT 边）
 */
int ev_fsm_init(ev_fsm_t *fsm, int initial_state, const ev_fsm_transition_t *transitions, size_t transition_count, const ev_fsm_state_t *states, size_t state_count, void *user_data, ev_fsm_free_arg_fn free_arg);

/**
 * @brief  清队（逐项 free_arg）并摘空表指针；不释放 fsm 本身
 * @param  fsm  状态机
 * @return 无
 *
 * 须在非 poll 中调用；若已 register，须先 ev_loop_unregister_fsm。
 * 若 fsm->is_polling（含 action/enter/exit 栈上）：直接返回且**不清队**——视为误用。
 * 正在 poll / action / enter / exit 内禁止 free 本 fsm。
 */
void ev_fsm_deinit(ev_fsm_t *fsm);

/******************************************************************************/
/* 事件队列                                                                   */
/******************************************************************************/

/**
 * @brief  投递事件，不立刻迁移
 * @param  fsm    状态机
 * @param  event  事件号
 * @param  arg    可选载荷；入队成功后所有权归库，经 free_arg 释放（action 不可接管）
 * @return 0 成功，队列满返回 -1（并增加 post_dropped，且经 free_arg 释放 arg）
 */
int ev_fsm_post(ev_fsm_t *fsm, int event, void *arg);

/**
 * @brief  消化队列直至空；重入调用直接返回
 * @param  fsm  状态机
 * @return 无
 *
 * 迁移顺序：on_exit → action → 更新 state → on_enter。
 * action 内 get_state() 仍为旧状态；action 返回后库仍会 free_arg(arg)。
 * action 内禁止再 poll；勿在回调里 free 正在 poll 的本 fsm。
 * 步数触达 EV_FSM_POLL_MAX_STEPS 时清空队列并投入 EV_FSM_FAULT，同一次 while 继续消化。
 * 处理 FAULT 时先清积压再 apply；FAULT 边 post 的事件保留到下一次 poll。
 * 推荐由 ev_loop_register_fsm 在回合末统一调用，业务回调里只 post。
 */
void ev_fsm_poll(ev_fsm_t *fsm);

/**
 * @brief  当前待处理事件个数
 * @param  fsm  状态机
 * @return 队列深度；fsm 为 NULL 时返回 0
 */
size_t ev_fsm_queue_len(const ev_fsm_t *fsm);

/******************************************************************************/
/* 状态查询                                                                   */
/******************************************************************************/

/**
 * @brief  取当前状态
 * @param  fsm  状态机
 * @return 状态编号；fsm 为 NULL 时返回 -1
 */
int ev_fsm_get_state(const ev_fsm_t *fsm);

/**
 * @brief  取当前状态名
 * @param  fsm  状态机
 * @return 名称，无表时为 "?"
 */
const char *ev_fsm_state_name(const ev_fsm_t *fsm);

#ifdef __cplusplus
}
#endif

#endif
