/**
 * @file    ev_work.h
 * @brief   轻量 offload 插件：临时线程跑 work，完成后 ev_loop_post 回 target_loop
 *
 * 与 ev_pool 同族、更瘦：无队列、无 worker 复用；适合偶发阻塞活。
 * 持续多任务、需限并发时用 ev_pool。
 *
 * 工作线程禁止直接调 ev_fsm_* / ev_io_* / ev_timer_* / ev_bus_*；
 * 业务推进放在 done（成功时在 target_loop 线程）。
 *
 * ---- 内存 ----
 * run 成功：user_data 交托管，直至 done 返回后经 free_fn 释放。
 * run 失败（未运行 / 建线程失败 / 参数非法）：不接管 user_data。
 * done 非 NULL 时必须提供 target_loop；投递包装走 ev_loop_post_full。
 *
 * ---- done / status ----
 * - status==0：work 已完成，且在 target_loop 线程调用 done。
 * - status!=0：work 已完成，但投递失败；done 在临时线程调用，仍禁止碰
 *   fsm/io/timer/bus；随后同线程调 free_fn。done_dropped++。
 *
 * ---- 停机 ----
 * deinit 前须 drain（deinit 内也会 drain）：等待在途线程结束后再拆 loop。
 */
#ifndef EV_WORK_H
#define EV_WORK_H

#include "evkit/ev_loop.h"

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* 类型                                                                       */
/******************************************************************************/

/** 临时线程执行；可阻塞 */
typedef void (*ev_work_fn)(void *user_data);

/**
 * status==0：在 target_loop 线程；status!=0：投递失败，在临时线程。
 */
typedef void (*ev_work_done_fn)(void *user_data, int status);

typedef struct ev_work_svc {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int inflight; // 在途临时线程数
    int running;  // 1=接受 run
    uint32_t done_dropped;
} ev_work_svc_t;

/******************************************************************************/
/* API                                                                        */
/******************************************************************************/

/**
 * @brief  初始化服务（同步原语）；不创建常驻线程
 * @return 0 成功，-1 失败
 */
int ev_work_init(ev_work_svc_t *svc);

/**
 * @brief  停收 run，drain 在途，销毁锁/条件变量；不释放 svc 本身
 */
void ev_work_deinit(ev_work_svc_t *svc);

/**
 * @brief  阻塞直至在途为 0
 * @return 0 成功，-1 参数非法
 */
int ev_work_drain(ev_work_svc_t *svc);

/**
 * @brief  当前在途任务数（调试用）
 */
int ev_work_inflight(ev_work_svc_t *svc);

/**
 * @brief  起临时线程执行 work，完成后 post 回 target_loop
 * @param  target_loop  done 非 NULL 时必填
 * @param  work         不可 NULL
 * @param  done         可 NULL
 * @param  free_fn      可 NULL；与 ev_loop_post_full 同义
 * @return 0 成功，-1 失败；失败不接管 user_data
 */
int ev_work_run(ev_work_svc_t *svc, ev_loop_t *target_loop, ev_work_fn work, ev_work_done_fn done, void *user_data, ev_post_free_fn free_fn);

#ifdef __cplusplus
}
#endif

#endif
