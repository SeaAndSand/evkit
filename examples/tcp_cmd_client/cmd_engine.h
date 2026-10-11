/**
 * @file    cmd_engine.h
 * @brief   命令引擎：解析请求 → 分发 handler → 统一回包
 *
 * sync：handler 内直接 cmd_req_reply_result / cmd_req_reply_error
 * async：handler 返回 0 表示已受理；引擎先发 ack；后续用 cmd_job_* 回 partial/result
 */
#ifndef TCP_CMD_ENGINE_H
#define TCP_CMD_ENGINE_H

#include "cJSON.h"
#include "session.h"

#include <stddef.h>

struct cmd_engine;
struct cmd_job;

typedef enum {
    CMD_MODE_SYNC = 0,
    CMD_MODE_ASYNC = 1
} cmd_mode_t;

typedef struct {
    struct cmd_engine *engine;
    char id[PROTO_MAX_ID_LEN];
    char cmd[PROTO_MAX_CMD_LEN];
    cmd_mode_t mode;
    cJSON *args; /* 可为 NULL；勿跨回调持有（root 在 on_line 末尾释放） */
    int replied; /* sync：handler 是否已 reply_* */
} cmd_req_t;

typedef int (*cmd_sync_fn)(cmd_req_t *req);
typedef int (*cmd_async_fn)(cmd_req_t *req, struct cmd_job **out_job);

typedef struct {
    const char *name;
    cmd_mode_t mode;
    cmd_sync_fn sync_fn;
    cmd_async_fn async_fn;
} cmd_handler_t;

typedef struct cmd_engine {
    session_t *session;
    ev_loop_t *loop;
    const cmd_handler_t *handlers;
    size_t handler_count;
    struct cmd_job *jobs[PROTO_MAX_PENDING];
    int job_count;
    uint32_t bad_json;
    uint32_t bad_request;
    uint32_t unknown_cmd;
    uint32_t mode_mismatch;
    uint32_t pending_full;
    uint32_t replied;
} cmd_engine_t;

int cmd_engine_init(cmd_engine_t *e, session_t *session, ev_loop_t *loop, const cmd_handler_t *handlers, size_t handler_count);
void cmd_engine_deinit(cmd_engine_t *e);
void cmd_engine_on_line(session_t *s, const char *line, void *user_data);
void cmd_engine_on_offline(session_t *s, void *user_data);

int cmd_req_reply_result(cmd_req_t *req, cJSON *data);
int cmd_req_reply_error(cmd_req_t *req, const char *code, const char *msg);

typedef struct cmd_job {
    cmd_engine_t *engine;
    char id[PROTO_MAX_ID_LEN];
    char cmd[PROTO_MAX_CMD_LEN];
    int active;
    int seq;
    void *user_data; /* async：常由 worker 填 task 指针 */
    void (*on_cancel)(struct cmd_job *job);
} cmd_job_t;

struct cmd_job *cmd_job_create(cmd_engine_t *e, const cmd_req_t *req, void (*on_cancel)(struct cmd_job *job), void *user_data);
struct cmd_job *cmd_job_find(cmd_engine_t *e, const char *id);
int cmd_job_reply_partial(struct cmd_job *job, cJSON *data);
int cmd_job_reply_result(struct cmd_job *job, cJSON *data);
int cmd_job_reply_error(struct cmd_job *job, const char *code, const char *msg);
void cmd_job_cancel(struct cmd_job *job);

#endif
