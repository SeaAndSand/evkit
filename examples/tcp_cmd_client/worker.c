/**
 * @file    worker.c
 * @brief   工作线程：独立 ev_loop + 每任务一台 FSM；与网络线程只经 post 往来
 *
 * 任务生命周期（为何不能「发完最后一包就 free」）:
 *   1) 工作线程 post 回包到网络 loop 时 pending_replies++
 *   2) 网络线程处理完回包后 pending_replies--
 *   3) 工作侧不再产生新回包时置 replies_done（或 cancelled）
 *   4) 仅当 pending_replies==0 且 (replies_done|cancelled) 时，在工作线程 free
 * 这样保证：网络侧还在用 task（查 job_id / 读 cancelled）时，task 一定还活着。
 *
 * 所有权:
 *   - task / timer / fsm 只在工作线程释放
 *   - job->user_data 在 submit 时指向 task；cancel/最终回包时由网络侧摘空
 *   - 回包用 job id 查找，避免 cancel 后悬空 job 指针
 */
#include "worker.h"

#include "evkit/ev_fsm.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    ST_IDLE = 0,
    ST_RUNNING,
    ST_COUNT
};

enum {
    EV_START = 1,
    EV_TICK,
    EV_ABORT
};

typedef enum {
    TASK_SLOW = 1,
    TASK_COUNT = 2
} task_kind_t;

typedef struct {
    task_kind_t kind;
    atomic_int cancelled;
    atomic_int pending_replies; /* 已 post 到网络、尚未处理完的回包数 */
    atomic_int replies_done;    /* 工作侧不再产生新回包 */
    atomic_int free_claimed;    /* CAS：保证只 free 一次 */
    ev_loop_t *net_loop;
    ev_loop_t *worker_loop;
    cmd_engine_t *engine;
    char job_id[PROTO_MAX_ID_LEN];
    ev_fsm_t fsm;
    ev_timer_t timer;
    int delay_ms;
    int count_total;
    int count_index;
    uint32_t interval_ms;
} task_t;

typedef struct {
    task_t *task;
    cJSON *data;
    int is_partial;
} reply_to_net_t;

static ev_loop_t g_worker_loop = {0};
static pthread_t g_worker_tid;
static int g_worker_running = 0;

static void worker_task_free(task_t *task)
{
    if (task == NULL) {
        return;
    }
    (void)ev_loop_unregister_fsm(task->worker_loop, &task->fsm);
    ev_fsm_deinit(&task->fsm);
    (void)ev_timer_remove(&task->timer);
    free(task);
}

/** 条件满足则在工作线程释放 task；任何路径收尾都走这里 */
static void worker_task_maybe_release(void *user_data)
{
    task_t *task = user_data;
    if (task == NULL) {
        return;
    }
    if (atomic_load(&task->pending_replies) > 0) {
        return;
    }
    if (!atomic_load(&task->cancelled) && !atomic_load(&task->replies_done)) {
        return;
    }
    int expected = 0;
    if (!atomic_compare_exchange_strong(&task->free_claimed, &expected, 1)) {
        return;
    }
    worker_task_free(task);
}

static void worker_task_schedule_release(task_t *task)
{
    (void)ev_loop_post(task->worker_loop, worker_task_maybe_release, task);
}

static void worker_mark_no_more_replies(task_t *task)
{
    atomic_store(&task->replies_done, 1);
}

static void worker_on_deliver_reply(void *user_data)
{
    reply_to_net_t *r = user_data;
    task_t *task = r->task;
    cJSON *data = r->data;
    int is_partial = r->is_partial;
    free(r);

    struct cmd_job *job = NULL;
    if (!atomic_load(&task->cancelled)) {
        job = cmd_job_find(task->engine, task->job_id);
    }

    if (job == NULL) {
        cJSON_Delete(data);
    } else if (is_partial) {
        (void)cmd_job_reply_partial(job, data);
    } else {
        job->on_cancel = NULL;
        job->user_data = NULL;
        (void)cmd_job_reply_result(job, data);
    }

    int left = atomic_fetch_sub(&task->pending_replies, 1) - 1;
    if (left == 0 && (atomic_load(&task->replies_done) || atomic_load(&task->cancelled))) {
        worker_task_schedule_release(task);
    }
}

static void worker_post_reply_to_net(task_t *task, cJSON *data, int is_partial, int is_final)
{
    if (atomic_load(&task->cancelled)) {
        cJSON_Delete(data);
        return;
    }

    reply_to_net_t *r = calloc(1, sizeof(*r));
    if (r == NULL) {
        cJSON_Delete(data);
        return;
    }
    r->task = task;
    r->data = data;
    r->is_partial = is_partial;
    /* 先于 post 标记：网络侧可能立刻处理回包，必须已能判定「可释放」 */
    if (is_final) {
        worker_mark_no_more_replies(task);
    }
    atomic_fetch_add(&task->pending_replies, 1);
    if (ev_loop_post(task->net_loop, worker_on_deliver_reply, r) != 0) {
        atomic_fetch_sub(&task->pending_replies, 1);
        cJSON_Delete(data);
        free(r);
        if (is_final) {
            worker_task_schedule_release(task);
        }
        return;
    }
}

static void worker_on_abort(void *user_data)
{
    task_t *task = user_data;
    if (task == NULL) {
        return;
    }
    atomic_store(&task->cancelled, 1);
    worker_mark_no_more_replies(task);
    (void)ev_timer_stop(&task->timer);
    worker_task_schedule_release(task);
}

static void worker_task_post_event(task_t *task, int event)
{
    if (ev_fsm_post(&task->fsm, event, NULL) != 0) {
        fprintf(stderr, "[worker] fsm overflow kind=%d\n", (int)task->kind);
    }
}

static void worker_on_task_timer(ev_loop_t *loop, ev_timer_t *timer)
{
    (void)loop;
    task_t *task = timer->user_data;
    worker_task_post_event(task, EV_TICK);
}

static void worker_enter_running(void *user_data, int state)
{
    (void)state;
    task_t *task = user_data;
    if (atomic_load(&task->cancelled)) {
        worker_task_post_event(task, EV_ABORT);
        return;
    }

    if (task->kind == TASK_SLOW) {
        ev_timer_start(task->worker_loop, &task->timer, (uint32_t)task->delay_ms, 0, worker_on_task_timer, task);
        printf("[worker] slow START delay=%d (worker loop+FSM)\n", task->delay_ms);
        return;
    }

    ev_timer_start(task->worker_loop, &task->timer, task->interval_ms, task->interval_ms, worker_on_task_timer, task);
    printf("[worker] count START n=%d interval=%u (worker loop+FSM)\n", task->count_total, task->interval_ms);
}

static void worker_exit_running(void *user_data, int state)
{
    (void)state;
    task_t *task = user_data;
    (void)ev_timer_stop(&task->timer);
}

static void worker_tick_slow(task_t *task)
{
    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddStringToObject(data, "status", "done");
        cJSON_AddStringToObject(data, "via", "worker_thread");
    }
    worker_post_reply_to_net(task, data, 0, 1);
    worker_task_post_event(task, EV_ABORT); /* RUNNING → IDLE，exit 停表 */
}

static void worker_tick_count(task_t *task)
{
    task->count_index++;
    cJSON *part = cJSON_CreateObject();
    if (part != NULL) {
        cJSON_AddNumberToObject(part, "i", task->count_index);
        cJSON_AddNumberToObject(part, "n", task->count_total);
        cJSON_AddStringToObject(part, "via", "worker_thread");
    }
    worker_post_reply_to_net(task, part, 1, 0);

    if (task->count_index < task->count_total) {
        return;
    }

    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddNumberToObject(data, "n", task->count_total);
        cJSON_AddStringToObject(data, "status", "complete");
        cJSON_AddStringToObject(data, "via", "worker_thread");
    }
    worker_post_reply_to_net(task, data, 0, 1);
    worker_task_post_event(task, EV_ABORT);
}

static void worker_action_tick(void *user_data, int event, void *arg)
{
    (void)event;
    (void)arg;
    task_t *task = user_data;
    if (atomic_load(&task->cancelled)) {
        return;
    }
    if (task->kind == TASK_SLOW) {
        worker_tick_slow(task);
    } else {
        worker_tick_count(task);
    }
}

/* 边表需要 action 槽；真正收尾靠 exit_running 停表 + abort 路径 schedule_release */
static void worker_action_abort(void *user_data, int event, void *arg)
{
    (void)user_data;
    (void)event;
    (void)arg;
}

static const ev_fsm_transition_t k_task_edges[] = {
    { ST_IDLE, EV_START, ST_RUNNING, NULL },
    { ST_RUNNING, EV_TICK, EV_FSM_STAY, worker_action_tick },
    { ST_RUNNING, EV_ABORT, ST_IDLE, worker_action_abort },
    { ST_IDLE, EV_ABORT, EV_FSM_STAY, worker_action_abort },
    { EV_FSM_ANY, EV_FSM_FAULT, ST_IDLE, worker_action_abort },
};

static const ev_fsm_state_t k_task_states[ST_COUNT] = {
    [ST_IDLE] = { .name = "IDLE", .on_enter = NULL, .on_exit = NULL },
    [ST_RUNNING] = { .name = "RUNNING", .on_enter = worker_enter_running, .on_exit = worker_exit_running },
};

static void worker_on_start_task(void *user_data)
{
    task_t *task = user_data;
    if (task == NULL) {
        return;
    }
    if (atomic_load(&task->cancelled)) {
        worker_mark_no_more_replies(task);
        worker_task_schedule_release(task);
        return;
    }
    worker_task_post_event(task, EV_START);
}

static task_t *worker_task_new(task_kind_t kind, ev_loop_t *net_loop, struct cmd_job *job)
{
    task_t *task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return NULL;
    }
    task->kind = kind;
    atomic_init(&task->cancelled, 0);
    atomic_init(&task->pending_replies, 0);
    atomic_init(&task->replies_done, 0);
    atomic_init(&task->free_claimed, 0);
    task->net_loop = net_loop;
    task->worker_loop = &g_worker_loop;
    task->engine = job->engine;
    snprintf(task->job_id, sizeof(task->job_id), "%s", job->id);
    if (ev_fsm_init(&task->fsm, ST_IDLE, k_task_edges, sizeof(k_task_edges) / sizeof(k_task_edges[0]),
                    k_task_states, ST_COUNT, task, NULL) != 0) {
        free(task);
        return NULL;
    }
    if (ev_loop_register_fsm(task->worker_loop, &task->fsm) != 0) {
        ev_fsm_deinit(&task->fsm);
        free(task);
        return NULL;
    }
    return task;
}

static int worker_task_submit(task_t *task)
{
    return ev_loop_post(&g_worker_loop, worker_on_start_task, task);
}

int worker_submit_slow(ev_loop_t *net_loop, struct cmd_job *job, int delay_ms)
{
    task_t *task = worker_task_new(TASK_SLOW, net_loop, job);
    if (task == NULL) {
        return -1;
    }
    task->delay_ms = delay_ms;
    job->user_data = task;
    if (worker_task_submit(task) != 0) {
        job->user_data = NULL;
        free(task);
        return -1;
    }
    return 0;
}

int worker_submit_count(ev_loop_t *net_loop, struct cmd_job *job, int n, int interval_ms)
{
    task_t *task = worker_task_new(TASK_COUNT, net_loop, job);
    if (task == NULL) {
        return -1;
    }
    task->count_total = n;
    task->count_index = 0;
    task->interval_ms = (uint32_t)interval_ms;
    job->user_data = task;
    if (worker_task_submit(task) != 0) {
        job->user_data = NULL;
        free(task);
        return -1;
    }
    return 0;
}

void worker_cancel_job(struct cmd_job *job)
{
    if (job == NULL || job->user_data == NULL) {
        return;
    }
    task_t *task = job->user_data;
    atomic_store(&task->cancelled, 1);
    job->user_data = NULL;
    if (ev_loop_post(task->worker_loop, worker_on_abort, task) != 0) {
        fprintf(stderr, "[worker] cancel post failed\n");
    }
}

static void *worker_thread_main(void *arg)
{
    (void)arg;
    printf("[worker] thread loop running\n");
    (void)ev_loop_run(&g_worker_loop);
    printf("[worker] thread loop exit\n");
    return NULL;
}

int worker_start(void)
{
    if (g_worker_running) {
        return 0;
    }
    memset(&g_worker_loop, 0, sizeof(g_worker_loop));
    if (ev_loop_init(&g_worker_loop) != 0) {
        return -1;
    }
    if (pthread_create(&g_worker_tid, NULL, worker_thread_main, NULL) != 0) {
        ev_loop_deinit(&g_worker_loop);
        return -1;
    }
    g_worker_running = 1;
    return 0;
}

void worker_stop(void)
{
    if (!g_worker_running) {
        return;
    }
    ev_loop_stop(&g_worker_loop);
    (void)pthread_join(g_worker_tid, NULL);
    ev_loop_deinit(&g_worker_loop);
    memset(&g_worker_tid, 0, sizeof(g_worker_tid));
    g_worker_running = 0;
}

ev_loop_t *worker_loop(void)
{
    return g_worker_running ? &g_worker_loop : NULL;
}
