/**
 * @file    ev_task.h
 * @brief   异步任务门面：统一结果句柄；执行后端可选 ev_work / ev_pool
 *
 * 简化上层写法：业务只认 task（return 结果 → loop 上 done），
 * 偶发用 work、限并用 pool，换后端不改 work/done 形状。
 *
 * 灵感来自 GTask 的「任务对象」，不是 GLib 兼容层；无 cancel / GObject。
 *
 * ---- 生命周期 ----
 * new 成功后：run 成功则所有权交给库，done 返回后 free_user(user_data) 并释放 task；
 * run 失败或尚未 run：调用方 ev_task_free（会调 free_user）。
 *
 * ---- 线程 ----
 * work 回调在后端线程；仅可 ev_task_return_* / 读写自有数据。
 * done 在 target_loop（投递失败时在后端线程，status 反映投递失败）。
 * work/done 内禁止直接调对方后端未允许的 fsm/io/timer/bus（同 work/pool）。
 */
#ifndef EV_TASK_H
#define EV_TASK_H

#include "evkit/ev_loop.h"
#include "evkit/ev_pool.h"
#include "evkit/ev_work.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ev_task ev_task_t;

/** 后端线程执行；通过 ev_task_return_* 填写结果（可省略，等价 return NULL/0） */
typedef void (*ev_task_work_fn)(ev_task_t *task);

/** 完成回调；用 get_ptr / get_status 取结果 */
typedef void (*ev_task_done_fn)(ev_task_t *task);

/**
 * @brief  分配任务并绑定 loop / done
 * @param  free_user  可 NULL；在 done 之后或 ev_task_free 时释放 user_data
 * @return 任务指针；失败 NULL
 */
ev_task_t *ev_task_new(ev_loop_t *loop, ev_task_done_fn done, void *user_data, ev_post_free_fn free_user);

/**
 * @brief  释放尚未成功 run 的任务（成功 run 后勿调用）
 */
void ev_task_free(ev_task_t *task);

/** 选择 ev_work 后端（与 use_pool 互斥，后者覆盖前者） */
int ev_task_use_work(ev_task_t *task, ev_work_svc_t *work);

/** 选择 ev_pool 后端 */
int ev_task_use_pool(ev_task_t *task, ev_pool_t *pool);

/**
 * @brief  提交到已选后端执行
 * @return 0 成功（此后由库释放 task）；-1 失败（调用方 ev_task_free）
 */
int ev_task_run(ev_task_t *task, ev_task_work_fn work);

/**
 * @brief  在 work 线程填写指针结果与业务 status（0=成功）
 * @note   仅存储；完成投递由后端在 work 返回后触发。重复调用以最后一次为准。
 */
void ev_task_return_ptr(ev_task_t *task, void *result, int status);

void *ev_task_get_ptr(const ev_task_t *task);
int ev_task_get_status(const ev_task_t *task);
void *ev_task_get_user(const ev_task_t *task);
ev_loop_t *ev_task_get_loop(const ev_task_t *task);

#ifdef __cplusplus
}
#endif

#endif
