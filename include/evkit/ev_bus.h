/**
 * @file    ev_bus.h
 * @brief   通用观察者总线：一对多扇出；匹配规则由应用提供
 *
 * 库负责：订阅表、EV_BUS_SYNC / EV_BUS_POST、按 match 命中后投递。
 * 应用负责：topic / match_arg 含义、match 实现、payload 类型与存活。
 *
 * 两种投递模式（订阅时选定）:
 * - EV_BUS_SYNC：在 publish 所在线程（分发 / owner 线程）直接回调
 * - EV_BUS_POST：ev_loop_post 到订阅者的 target_loop，在对方线程回调
 *
 * 线程契约：init/deinit/subscribe/unsubscribe/publish 仅 owner_loop 线程。
 * 跨线程发布：先 post 到 owner，再在回调里 publish。
 *
 * ---- 内存（与 ev_loop_post 同一模型：库不接管业务指针）----
 * topic / payload：库不拷贝、不释放。
 * - SYNC：仅回调期间有效（可用栈上对象）
 * - POST：须活到该订阅者回调执行完
 * - 一次 publish 命中多个 POST：指针须活到全部相关回调结束；
 *   禁止在其中一个回调里 free 共享堆，禁止用即将返回的栈缓冲。
 * 推荐：环缓下标 / 堆块唯一释放者 / 能 SYNC 则 SYNC。详见 README「资源所有权」。
 * match_arg：须活到 unsubscribe（勿用订完即失效的栈变量）。
 * POST 的 delivery 包装经 ev_loop_post_full(..., free_fn) 投递：正常回调后或目标 loop
 * deinit 丢弃时由库释放包装块（topic/payload 仍始终由应用保活）。
 */
#ifndef EV_BUS_H
#define EV_BUS_H

#include "evkit/ev_loop.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* 常量                                                                       */
/******************************************************************************/

#define EV_BUS_SUB_MAX 32 // 单 bus 最大订阅数

/******************************************************************************/
/* 类型                                                                       */
/******************************************************************************/

typedef enum {
    EV_BUS_SYNC = 0, // 分发线程直接回调
    EV_BUS_POST = 1  // 投递到订阅者 loop
} ev_bus_mode_t;

/** 非 0 = 命中；topic 来自 publish，match_arg 来自 subscribe */
typedef int (*ev_bus_match_fn)(const void *topic, const void *match_arg, void *user_data);

/** 命中后的业务回调；topic 与 publish 传入的为同一指针 */
typedef void (*ev_bus_cb)(const void *topic, void *payload, void *user_data);

typedef struct {
    int sub_id;                 // >=1；0 表示空槽（内部）
    ev_bus_mode_t mode;         // SYNC / POST
    ev_loop_t *target_loop;     // POST 必填；SYNC 为 NULL
    ev_bus_cb cb;               // 回调
    void *user_data;            // 业务上下文（亦传给 match）
    ev_bus_match_fn match;      // 匹配函数
    const void *match_arg;      // 传给 match 的订阅侧参数（应用解释；订期内须有效）
} ev_bus_sub_t;

typedef struct ev_bus {
    ev_loop_t *owner_loop; // 改表与 publish 所在 loop
    ev_bus_sub_t subs[EV_BUS_SUB_MAX];
    int sub_count;         // 当前订阅数
    int next_sub_id;       // 自增 id，从 1 起
    uint32_t post_dropped; // POST 时 loop_post 失败累计
} ev_bus_t;

/******************************************************************************/
/* 库提供的通用 match（精确 u32）                                             */
/******************************************************************************/

/** topic / match_arg 均视为 const uint32_t*；相等则命中 */
int ev_bus_match_u32(const void *topic, const void *match_arg, void *user_data);

/******************************************************************************/
/* API                                                                        */
/******************************************************************************/

/**
 * @brief  初始化 bus，绑定 owner_loop
 * @return 0 成功，-1 失败
 */
int ev_bus_init(ev_bus_t *bus, ev_loop_t *owner_loop);

/**
 * @brief  清空订阅表；不取消各 loop 上未执行的 POST delivery
 */
void ev_bus_deinit(ev_bus_t *bus);

/**
 * @brief  订阅
 * @param  mode          EV_BUS_SYNC 或 EV_BUS_POST
 * @param  target_loop   SYNC 传 NULL；POST 必非 NULL
 * @param  match         匹配函数，不可 NULL；简单场景用 ev_bus_match_u32
 * @param  match_arg     传给 match 的订阅侧参数；可为 NULL（若 match 允许）；订期内须有效
 * @return sub_id (>=1) 成功，-1 失败
 */
int ev_bus_subscribe(ev_bus_t *bus, ev_bus_mode_t mode, ev_loop_t *target_loop, ev_bus_cb cb, void *user_data, ev_bus_match_fn match, const void *match_arg);

/**
 * @brief  按 sub_id 退订；不撤销已入队的 POST
 * @return 0 成功，-1 未找到
 */
int ev_bus_unsubscribe(ev_bus_t *bus, int sub_id);

/**
 * @brief  发布；仅 owner 线程
 * @param  topic  传给各订阅 match / 命中后的 cb；可为 NULL（若 match 允许）
 * @return 成功分发次数（SYNC 调用次数 + POST 入队成功次数）
 *
 * 单个 POST 队列满：跳过该订阅，post_dropped++，继续其余订阅。
 * publish 先整表拷贝再 match/分发（match 内改表不影响本次）。
 * POST 保活见文件头与 README「资源所有权」。
 */
int ev_bus_publish(ev_bus_t *bus, const void *topic, void *payload);

#ifdef __cplusplus
}
#endif

#endif
