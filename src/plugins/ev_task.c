/**
 * @file    ev_task.c
 * @brief   异步任务门面实现
 */
#define _DEFAULT_SOURCE
#include "evkit/ev_task.h"

#include <stdlib.h>
#include <string.h>

typedef enum {
    EV_TASK_BACKEND_NONE = 0,
    EV_TASK_BACKEND_WORK = 1,
    EV_TASK_BACKEND_POOL = 2
} ev_task_backend_t;

struct ev_task {
    ev_loop_t *loop;
    ev_task_work_fn work_fn;
    ev_task_done_fn done_fn;
    void *user_data;
    ev_post_free_fn free_fn;

    void *result;
    int status;   // 业务 status（return_ptr）；投递失败时改为非 0
    int returned; // 是否调用过 return_ptr
    int started;  // run 已成功提交

    ev_task_backend_t backend;
    ev_work_t *work;
    ev_pool_t *pool;
};

ev_task_t *ev_task_new(ev_loop_t *loop, ev_task_done_fn done, void *user_data, ev_post_free_fn free_fn)
{
    if (loop == NULL) {
        return NULL;
    }
    ev_task_t *task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return NULL;
    }
    task->loop = loop;
    task->done_fn = done;
    task->user_data = user_data;
    task->free_fn = free_fn;
    return task;
}

void ev_task_free(ev_task_t *task)
{
    if (task == NULL || task->started) {
        return;
    }
    if (task->free_fn != NULL) {
        task->free_fn(task->user_data);
    }
    free(task);
}

int ev_task_use_work(ev_task_t *task, ev_work_t *work)
{
    if (task == NULL || work == NULL || task->started) {
        return -1;
    }
    task->backend = EV_TASK_BACKEND_WORK;
    task->work = work;
    task->pool = NULL;
    return 0;
}

int ev_task_use_pool(ev_task_t *task, ev_pool_t *pool)
{
    if (task == NULL || pool == NULL || task->started) {
        return -1;
    }
    task->backend = EV_TASK_BACKEND_POOL;
    task->pool = pool;
    task->work = NULL;
    return 0;
}

void ev_task_return_ptr(ev_task_t *task, void *result, int status)
{
    if (task == NULL) {
        return;
    }
    task->result = result;
    task->status = status;
    task->returned = 1;
}

void *ev_task_get_ptr(const ev_task_t *task)
{
    return (task != NULL) ? task->result : NULL;
}

int ev_task_get_status(const ev_task_t *task)
{
    return (task != NULL) ? task->status : -1;
}

void *ev_task_get_user(const ev_task_t *task)
{
    return (task != NULL) ? task->user_data : NULL;
}

ev_loop_t *ev_task_get_loop(const ev_task_t *task)
{
    return (task != NULL) ? task->loop : NULL;
}

static void ev_task_bridge_work(void *user_data)
{
    ev_task_t *task = user_data;
    if (task->work_fn != NULL) {
        task->work_fn(task);
    }
    if (!task->returned) {
        ev_task_return_ptr(task, NULL, 0);
    }
}

static void ev_task_bridge_done(void *user_data, int deliver_status)
{
    ev_task_t *task = user_data;
    if (deliver_status != 0) {
        task->status = deliver_status;
    }
    if (task->done_fn != NULL) {
        task->done_fn(task);
    }
}

static void ev_task_bridge_free(void *user_data)
{
    ev_task_t *task = user_data;
    if (task->free_fn != NULL) {
        task->free_fn(task->user_data);
    }
    free(task);
}

int ev_task_run(ev_task_t *task, ev_task_work_fn work)
{
    if (task == NULL || work == NULL || task->started || task->loop == NULL) {
        return -1;
    }
    if (task->backend == EV_TASK_BACKEND_NONE) {
        return -1;
    }

    task->work_fn = work;

    int rc = -1;
    if (task->backend == EV_TASK_BACKEND_WORK) {
        rc = ev_work_submit(task->work, task->loop, ev_task_bridge_work, ev_task_bridge_done, task, ev_task_bridge_free);
    } else if (task->backend == EV_TASK_BACKEND_POOL) {
        rc = ev_pool_submit(task->pool, task->loop, ev_task_bridge_work, ev_task_bridge_done, task, ev_task_bridge_free);
    }

    if (rc != 0) {
        task->work_fn = NULL;
        return -1;
    }
    task->started = 1;
    return 0;
}

int ev_task_run_work(ev_task_t *task, ev_work_t *work, ev_task_work_fn work_fn)
{
    if (ev_task_use_work(task, work) != 0) {
        return -1;
    }
    return ev_task_run(task, work_fn);
}

int ev_task_run_pool(ev_task_t *task, ev_pool_t *pool, ev_task_work_fn work_fn)
{
    if (ev_task_use_pool(task, pool) != 0) {
        return -1;
    }
    return ev_task_run(task, work_fn);
}
