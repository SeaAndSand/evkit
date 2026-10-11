/**
 * @file    worker.h
 * @brief   工作线程：独立 ev_loop + 任务 FSM；与网络线程只经 ev_loop_post 往来
 *
 * 网络线程 ──post──► 工作 loop（跑 slow/count 的 FSM/timer）
 * 工作线程 ──post──► 网络 loop（cmd_job_reply_partial / result）
 *
 * task 释放时机见 worker.c 文件头「任务生命周期」。
 */
#ifndef TCP_CMD_WORKER_H
#define TCP_CMD_WORKER_H

#include "cmd_engine.h"
#include "evkit/ev_loop.h"

int worker_start(void);
void worker_stop(void);
ev_loop_t *worker_loop(void);

/** 在网络线程调用：把任务丢进工作 loop；job->user_data 由 worker 接管为 task* */
int worker_submit_slow(ev_loop_t *net_loop, struct cmd_job *job, int delay_ms);
int worker_submit_count(ev_loop_t *net_loop, struct cmd_job *job, int n, int interval_ms);

/** 供 job.on_cancel：仅网络线程；置取消标志并 post 到工作线程收尾 */
void worker_cancel_job(struct cmd_job *job);

#endif
