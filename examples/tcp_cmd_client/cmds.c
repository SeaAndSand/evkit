/**
 * @file    cmds.c
 * @brief   业务命令：sync 在网络线程；async 丢到工作线程 loop+FSM
 */
#include "cmds.h"

#include "evkit/ev_loop.h"
#include "worker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmd_ping(cmd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return cmd_req_reply_error(req, "NOMEM", "oom");
    }
    cJSON_AddBoolToObject(data, "pong", 1);
    return cmd_req_reply_result(req, data);
}

static int cmd_quit(cmd_req_t *req)
{
    cJSON *data = cJSON_CreateObject();
    if (data != NULL) {
        cJSON_AddStringToObject(data, "bye", "1");
    }
    int rc = cmd_req_reply_result(req, data);
    ev_loop_stop(req->engine->loop);
    return rc;
}

static int cmd_echo(cmd_req_t *req)
{
    const char *text = "";
    if (req->args != NULL) {
        cJSON *t = cJSON_GetObjectItemCaseSensitive(req->args, "text");
        if (cJSON_IsString(t) && t->valuestring != NULL) {
            text = t->valuestring;
        }
    }
    cJSON *data = cJSON_CreateObject();
    if (data == NULL) {
        return cmd_req_reply_error(req, "NOMEM", "oom");
    }
    cJSON_AddStringToObject(data, "text", text);
    return cmd_req_reply_result(req, data);
}

static int cmd_slow(cmd_req_t *req, struct cmd_job **out_job)
{
    int delay_ms = 500;
    if (req->args != NULL) {
        cJSON *d = cJSON_GetObjectItemCaseSensitive(req->args, "delay_ms");
        if (cJSON_IsNumber(d) && d->valuedouble > 0) {
            delay_ms = (int)d->valuedouble;
        }
    }
    if (delay_ms > 60000) {
        delay_ms = 60000;
    }

    struct cmd_job *job = cmd_job_create(req->engine, req, worker_cancel_job, NULL);
    if (job == NULL) {
        return -1;
    }
    if (worker_submit_slow(req->engine->loop, job, delay_ms) != 0) {
        job->on_cancel = NULL;
        cmd_job_cancel(job);
        return -1;
    }
    *out_job = job;
    return 0;
}

static int cmd_count(cmd_req_t *req, struct cmd_job **out_job)
{
    int n = 3;
    int interval_ms = 100;
    if (req->args != NULL) {
        cJSON *jn = cJSON_GetObjectItemCaseSensitive(req->args, "n");
        cJSON *ji = cJSON_GetObjectItemCaseSensitive(req->args, "interval_ms");
        if (cJSON_IsNumber(jn) && jn->valuedouble > 0) {
            n = (int)jn->valuedouble;
        }
        if (cJSON_IsNumber(ji) && ji->valuedouble > 0) {
            interval_ms = (int)ji->valuedouble;
        }
    }
    if (n > 100) {
        n = 100;
    }
    if (interval_ms > 10000) {
        interval_ms = 10000;
    }

    struct cmd_job *job = cmd_job_create(req->engine, req, worker_cancel_job, NULL);
    if (job == NULL) {
        return -1;
    }
    if (worker_submit_count(req->engine->loop, job, n, interval_ms) != 0) {
        job->on_cancel = NULL;
        cmd_job_cancel(job);
        return -1;
    }
    *out_job = job;
    return 0;
}

static const cmd_handler_t k_handlers[] = {
    { .name = "ping", .mode = CMD_MODE_SYNC, .sync_fn = cmd_ping, .async_fn = NULL },
    { .name = "echo", .mode = CMD_MODE_SYNC, .sync_fn = cmd_echo, .async_fn = NULL },
    { .name = "quit", .mode = CMD_MODE_SYNC, .sync_fn = cmd_quit, .async_fn = NULL },
    { .name = "slow", .mode = CMD_MODE_ASYNC, .sync_fn = NULL, .async_fn = cmd_slow },
    { .name = "count", .mode = CMD_MODE_ASYNC, .sync_fn = NULL, .async_fn = cmd_count },
};

const cmd_handler_t *cmds_handlers(size_t *out_count)
{
    if (out_count != NULL) {
        *out_count = sizeof(k_handlers) / sizeof(k_handlers[0]);
    }
    return k_handlers;
}
