/**
 * @file    ev_fsm.c
 * @brief   队列边表状态机实现
 */
#include "evkit/ev_fsm.h"

#include <string.h>

/******************************************************************************/
/* 内部：队列与匹配 */
/******************************************************************************/
/**
 * @brief  若设置了 free_arg，则释放载荷
 * @param  fsm  状态机
 * @param  arg  载荷
 * @return 无
 */
static void call_free_arg(ev_fsm_t *fsm, void *arg)
{
    if (arg != NULL && fsm->free_arg != NULL) {
        fsm->free_arg(arg);
    }
}

/**
 * @brief  清空事件队列，并对每项调用 free_arg
 * @param  fsm  状态机
 * @return 无
 */
static void queue_clear(ev_fsm_t *fsm)
{
    ev_fsm_queue_t *queue = &fsm->queue;
    while (queue->count > 0u) {
        ev_fsm_item_t item = queue->items[queue->head];
        queue->head = (uint8_t)((queue->head + 1u) % EV_FSM_QUEUE_CAPACITY);
        queue->count--;
        call_free_arg(fsm, item.arg);
    }
    queue->head = 0;
    queue->tail = 0;
}

/**
 * @brief  事件入队
 * @param  fsm    状态机
 * @param  event  事件号
 * @param  arg    可选载荷
 * @return 0 成功，满返回 -1
 */
static int queue_push(ev_fsm_t *fsm, int event, void *arg)
{
    ev_fsm_queue_t *queue = &fsm->queue;
    if (queue->count >= EV_FSM_QUEUE_CAPACITY) {
        return -1;
    }
    queue->items[queue->tail].event = event;
    queue->items[queue->tail].arg = arg;
    queue->tail = (uint8_t)((queue->tail + 1u) % EV_FSM_QUEUE_CAPACITY);
    queue->count++;
    return 0;
}

/**
 * @brief  事件出队
 * @param  fsm  状态机
 * @param  out  输出项
 * @return 1 有数据，0 空
 */
static int queue_pop(ev_fsm_t *fsm, ev_fsm_item_t *out)
{
    ev_fsm_queue_t *queue = &fsm->queue;
    if (queue->count == 0u) {
        return 0;
    }
    *out = queue->items[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) % EV_FSM_QUEUE_CAPACITY);
    queue->count--;
    return 1;
}

/**
 * @brief  边匹配分数：精确 > 半通配 > 双通配
 * @param  edge  边
 * @return 0..3，越大越具体
 */
static int edge_score(const ev_fsm_transition_t *edge)
{
    int score = 0;
    if (edge->from_state != EV_FSM_ANY) {
        score += 2;
    }
    if (edge->event != EV_FSM_ANY) {
        score += 1;
    }
    return score;
}

/**
 * @brief  查找迁移边；同分取表中先出现者
 * @param  fsm    状态机
 * @param  state  当前状态
 * @param  event  事件
 * @return 边指针，无匹配为 NULL
 */
static const ev_fsm_transition_t *find_transition(const ev_fsm_t *fsm, int state, int event)
{
    const ev_fsm_transition_t *best = NULL;
    int best_score = -1;
    for (size_t i = 0; i < fsm->transition_count; i++) {
        const ev_fsm_transition_t *edge = &fsm->transitions[i];
        int state_ok = (edge->from_state == EV_FSM_ANY) || (edge->from_state == state);
        int event_ok = (edge->event == EV_FSM_ANY) || (edge->event == event);
        if (!state_ok || !event_ok) {
            continue;
        }
        int score = edge_score(edge);
        if (score > best_score) {
            best = edge;
            best_score = score;
            if (score == 3) {
                return best;
            }
        }
    }
    return best;
}

/**
 * @brief  边表是否含全局 FAULT 收敛边（from_state==ANY && event==FAULT）
 * @param  transitions  边表
 * @param  count        边数量
 * @return 1 有，0 无
 */
static int has_fault_edge(const ev_fsm_transition_t *transitions, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (transitions[i].from_state == EV_FSM_ANY && transitions[i].event == EV_FSM_FAULT) {
            return 1;
        }
    }
    return 0;
}

/**
 * @brief  按编号取状态描述
 * @param  fsm    状态机
 * @param  state  状态编号
 * @return 描述指针，越界或无表为 NULL
 */
static const ev_fsm_state_t *state_desc(const ev_fsm_t *fsm, int state)
{
    if (fsm->states == NULL || state < 0 || (size_t)state >= fsm->state_count) {
        return NULL;
    }
    return &fsm->states[state];
}

/**
 * @brief  调用 on_exit
 * @param  fsm    状态机
 * @param  state  正离开的状态
 * @return 无
 */
static void call_exit(ev_fsm_t *fsm, int state)
{
    const ev_fsm_state_t *desc = state_desc(fsm, state);
    if (desc != NULL && desc->on_exit != NULL) {
        desc->on_exit(fsm->user_data, state);
    }
}

/**
 * @brief  调用 on_enter
 * @param  fsm    状态机
 * @param  state  正进入的状态
 * @return 无
 */
static void call_enter(ev_fsm_t *fsm, int state)
{
    const ev_fsm_state_t *desc = state_desc(fsm, state);
    if (desc != NULL && desc->on_enter != NULL) {
        desc->on_enter(fsm->user_data, state);
    }
}

/**
 * @brief  执行一条迁移：exit → action → 改 state → enter
 * @param  fsm    状态机
 * @param  edge   匹配到的边
 * @param  event  触发事件
 * @param  arg    载荷（仍由调用方 release）
 * @return 无
 */
static void apply_transition(ev_fsm_t *fsm, const ev_fsm_transition_t *edge, int event, void *arg)
{
    int old_state = fsm->state;
    int next_state = (edge->to_state == EV_FSM_STAY) ? old_state : edge->to_state;
    int changed = (next_state != old_state);

    if (changed) {
        call_exit(fsm, old_state);
    }
    if (edge->action != NULL) {
        edge->action(fsm->user_data, event, arg);
    }
    fsm->state = next_state;
    if (changed) {
        call_enter(fsm, next_state);
    }
}

/******************************************************************************/
/* 生命周期 */
/******************************************************************************/
/**
 * @brief  初始化状态机，并对初始状态调用 on_enter
 * @param  fsm                状态机
 * @param  initial_state      初始状态编号
 * @param  transitions        边表（须含 FAULT 边）
 * @param  transition_count   边数量
 * @param  states             可选状态描述
 * @param  state_count        states 元素数
 * @param  user_data          上下文
 * @param  free_arg           释放 arg 的回调，可 NULL
 * @return 0 成功，-1 失败
 */
int ev_fsm_init(ev_fsm_t *fsm, int initial_state, const ev_fsm_transition_t *transitions, size_t transition_count, const ev_fsm_state_t *states, size_t state_count, void *user_data, ev_fsm_free_arg_fn free_arg)
{
    if (fsm == NULL || transitions == NULL || transition_count == 0u || !has_fault_edge(transitions, transition_count)) {
        return -1;
    }
    memset(fsm, 0, sizeof(*fsm));
    fsm->state = initial_state;
    fsm->transitions = transitions;
    fsm->transition_count = transition_count;
    fsm->states = states;
    fsm->state_count = (states != NULL) ? state_count : 0;
    fsm->user_data = user_data;
    fsm->free_arg = free_arg;
    call_enter(fsm, initial_state);
    return 0;
}

/**
 * @brief  清队（逐项 free_arg）并摘空表指针；不释放 fsm 本身
 * @param  fsm  状态机
 * @return 无
 *
 * 若正在 poll 则直接返回。须先 unregister（若已注册）。
 */
void ev_fsm_deinit(ev_fsm_t *fsm)
{
    if (fsm == NULL || fsm->is_polling) {
        return;
    }
    queue_clear(fsm);
    fsm->transitions = NULL;
    fsm->transition_count = 0;
    fsm->states = NULL;
    fsm->state_count = 0;
    fsm->user_data = NULL;
    fsm->free_arg = NULL;
}

/******************************************************************************/
/* 事件队列 */
/******************************************************************************/
/**
 * @brief  投递事件，不立刻迁移
 * @param  fsm    状态机
 * @param  event  事件号
 * @param  arg    可选载荷；满时经 free_arg 释放
 * @return 0 成功，队列满返回 -1
 */
int ev_fsm_post(ev_fsm_t *fsm, int event, void *arg)
{
    if (fsm == NULL) {
        return -1;
    }
    if (queue_push(fsm, event, arg) != 0) {
        fsm->post_dropped++;
        call_free_arg(fsm, arg);
        return -1;
    }
    return 0;
}

/**
 * @brief  消化队列直至空；重入直接返回
 * @param  fsm  状态机
 * @return 无
 */
void ev_fsm_poll(ev_fsm_t *fsm)
{
    if (fsm == NULL || fsm->is_polling) {
        return;
    }
    fsm->is_polling = 1;

    uint32_t steps = 0;
    ev_fsm_item_t item = {0};
    while (queue_pop(fsm, &item)) {
        if (item.event == EV_FSM_FAULT) {
            /* 先清积压，再 apply：FAULT 边 post 的事件留给下一次 poll */
            queue_clear(fsm);
            const ev_fsm_transition_t *edge = find_transition(fsm, fsm->state, item.event);
            if (edge != NULL) {
                apply_transition(fsm, edge, item.event, item.arg);
            }
            call_free_arg(fsm, item.arg);
            break;
        }

        const ev_fsm_transition_t *edge = find_transition(fsm, fsm->state, item.event);
        if (edge == NULL) {
            call_free_arg(fsm, item.arg);
            continue;
        }
        apply_transition(fsm, edge, item.event, item.arg);
        call_free_arg(fsm, item.arg);

        steps++;
        if (steps >= EV_FSM_POLL_MAX_STEPS) {
            /* 触达步数上限：清队列并压入 FAULT，由本 while 继续消化 */
            fsm->poll_limit_hits++;
            queue_clear(fsm);
            (void)queue_push(fsm, EV_FSM_FAULT, NULL);
        }
    }

    fsm->is_polling = 0;
}

/******************************************************************************/
/* 状态查询 */
/******************************************************************************/
/**
 * @brief  取当前状态
 * @param  fsm  状态机
 * @return 状态编号
 */
int ev_fsm_get_state(const ev_fsm_t *fsm)
{
    if (fsm == NULL) {
        return -1;
    }
    return fsm->state;
}

/**
 * @brief  取当前状态名
 * @param  fsm  状态机
 * @return 名称，无表时为 "?"
 */
const char *ev_fsm_state_name(const ev_fsm_t *fsm)
{
    if (fsm == NULL) {
        return "?";
    }
    const ev_fsm_state_t *desc = state_desc(fsm, fsm->state);
    if (desc == NULL || desc->name == NULL) {
        return "?";
    }
    return desc->name;
}

/**
 * @brief  当前待处理事件个数
 * @param  fsm  状态机
 * @return 队列深度
 */
size_t ev_fsm_queue_len(const ev_fsm_t *fsm)
{
    if (fsm == NULL) {
        return 0;
    }
    return fsm->queue.count;
}
