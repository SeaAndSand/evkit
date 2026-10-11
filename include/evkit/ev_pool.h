/**
 * @file    ev_pool.h
 * @brief   线程池插件：有界队列 + N worker；完成后 ev_loop_post 回 target_loop
 *
 * 建在 evkit core 之上（与 ev_bus 同级）。worker 只执行 work，禁止直接调
 * ev_fsm_* / ev_io_* / ev_timer_* / ev_bus_*；业务推进放在 done 回调（target_loop 线程）。
 *
 * ---- 内存 ----
 * submit 成功：user_data 交池托管，直至 done 返回后经 free_fn 释放，或 shutdown discard
 * 时对未开跑任务调 free_fn（不调 done）。
 * submit 失败（满/已 shutdown）：不接管 user_data，调用者自行释放。
 * done 非 NULL 时必须提供 target_loop；投递包装走 ev_loop_post_full。
 *
 * ---- 停机 ----
 * EV_POOL_SHUTDOWN_DRAIN：停收新任务，跑完队列再 join。
 * EV_POOL_SHUTDOWN_DISCARD：停收；未开跑任务 free_fn，不调 done；再 join。
 */
#ifndef EV_POOL_H
#define EV_POOL_H

#include "evkit/ev_loop.h"

#include <pthread.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/******************************************************************************/
/* 常量                                                                       */
/******************************************************************************/

#define EV_POOL_WORKER_MAX 16
#define EV_POOL_QUEUE_CAP  64

typedef enum {
    EV_POOL_SHUTDOWN_DRAIN = 0,   // 排空已入队任务
    EV_POOL_SHUTDOWN_DISCARD = 1  // 丢弃未开跑任务（free_fn）
} ev_pool_shutdown_t;

/** worker 线程执行；可阻塞 */
typedef void (*ev_pool_work_fn)(void *user_data);

/**
 * target_loop 线程执行；status 恒为 0（成功跑完 work）。
 * 丢弃路径不调用 done。
 */
typedef void (*ev_pool_done_fn)(void *user_data, int status);

typedef struct ev_pool_job {
    ev_pool_work_fn work;
    ev_pool_done_fn done;
    void *user_data;
    ev_post_free_fn free_fn;
    ev_loop_t *target_loop;
} ev_pool_job_t;

typedef struct ev_pool {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    ev_pool_job_t q[EV_POOL_QUEUE_CAP];
    int head;
    int tail;
    int count;
    pthread_t workers[EV_POOL_WORKER_MAX];
    int nworkers;
    int running;   // 1=接受 submit
    int stopping;  // 1=worker 应在队列空后退出
    uint32_t submit_dropped;
    uint32_t jobs_discarded;
} ev_pool_t;

/******************************************************************************/
/* API                                                                        */
/******************************************************************************/

/**
 * @brief  创建 nworkers 个线程（1..EV_POOL_WORKER_MAX）
 * @return 0 成功，-1 失败
 */
int ev_pool_init(ev_pool_t *pool, int nworkers);

/**
 * @brief  停收任务并按 mode 处理队列，然后 join 全部 worker（可阻塞）
 * @return 0 成功，-1 参数非法
 *
 * 可重复调用；第二次为空操作。deinit 前应先 shutdown（否则 deinit 默认 DRAIN）。
 */
int ev_pool_shutdown(ev_pool_t *pool, ev_pool_shutdown_t mode);

/**
 * @brief  若尚未 shutdown 则 DRAIN；销毁锁/条件变量；不释放 pool 本身
 */
void ev_pool_deinit(ev_pool_t *pool);

/**
 * @brief  提交任务（任意线程可调）
 * @param  target_loop  done 非 NULL 时必填；done 与 free_fn 经 post_full 投递到此 loop
 * @param  work         不可 NULL
 * @param  done         可 NULL（仍可仅用 free_fn；无 target_loop 时在 worker 上 free）
 * @param  user_data    传给 work / done / free_fn
 * @param  free_fn      可 NULL；与 ev_loop_post_full 的 free_fn 同义
 * @return 0 成功，-1 失败（满 / 已停 / 参数非法）；失败不接管 user_data
 */
int ev_pool_submit(ev_pool_t *pool, ev_loop_t *target_loop, ev_pool_work_fn work, ev_pool_done_fn done, void *user_data,
                   ev_post_free_fn free_fn);

#ifdef __cplusplus
}
#endif

#endif
