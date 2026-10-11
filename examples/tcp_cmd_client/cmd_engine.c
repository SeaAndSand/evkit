/**
 * @file    cmd_engine.c
 * @brief   JSON 命令解析、分发与回包
 */
#include "cmd_engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmd_engine_send_json(cmd_engine_t *e, cJSON *obj)
{
    if (e == NULL || obj == NULL || e->session == NULL) {
        cJSON_Delete(obj);
        return -1;
    }
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (text == NULL) {
        return -1;
    }
    int rc = session_send_line(e->session, text);
    free(text);
    if (rc == 0) {
        e->replied++;
    }
    return rc;
}

static cJSON *cmd_make_reply_json(const char *id, const char *type, int ok)
{
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "id", id != NULL ? id : "");
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddBoolToObject(o, "ok", ok);
    return o;
}

static int cmd_engine_send_error(cmd_engine_t *e, const char *id, const char *code, const char *msg)
{
    cJSON *o = cmd_make_reply_json(id, "error", 0);
    if (o == NULL) {
        return -1;
    }
    cJSON *err = cJSON_CreateObject();
    if (err == NULL) {
        cJSON_Delete(o);
        return -1;
    }
    cJSON_AddStringToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg != NULL ? msg : "");
    cJSON_AddItemToObject(o, "error", err);
    return cmd_engine_send_json(e, o);
}

int cmd_req_reply_result(cmd_req_t *req, cJSON *data)
{
    if (req == NULL || req->engine == NULL) {
        cJSON_Delete(data);
        return -1;
    }
    cJSON *o = cmd_make_reply_json(req->id, "result", 1);
    if (o == NULL) {
        cJSON_Delete(data);
        return -1;
    }
    if (data != NULL) {
        cJSON_AddItemToObject(o, "data", data);
    } else {
        cJSON_AddObjectToObject(o, "data");
    }
    req->replied = 1;
    return cmd_engine_send_json(req->engine, o);
}

int cmd_req_reply_error(cmd_req_t *req, const char *code, const char *msg)
{
    if (req == NULL || req->engine == NULL) {
        return -1;
    }
    req->replied = 1;
    return cmd_engine_send_error(req->engine, req->id, code, msg);
}

static int cmd_job_slot_add(cmd_engine_t *e, struct cmd_job *job)
{
    if (e->job_count >= PROTO_MAX_PENDING) {
        return -1;
    }
    e->jobs[e->job_count++] = job;
    return 0;
}

static void cmd_job_slot_remove(cmd_engine_t *e, struct cmd_job *job)
{
    for (int i = 0; i < e->job_count; i++) {
        if (e->jobs[i] == job) {
            e->jobs[i] = e->jobs[e->job_count - 1];
            e->jobs[e->job_count - 1] = NULL;
            e->job_count--;
            return;
        }
    }
}

struct cmd_job *cmd_job_find(cmd_engine_t *e, const char *id)
{
    if (e == NULL || id == NULL) {
        return NULL;
    }
    for (int i = 0; i < e->job_count; i++) {
        if (e->jobs[i] != NULL && strcmp(e->jobs[i]->id, id) == 0) {
            return e->jobs[i];
        }
    }
    return NULL;
}

struct cmd_job *cmd_job_create(cmd_engine_t *e, const cmd_req_t *req, void (*on_cancel)(struct cmd_job *job), void *user_data)
{
    if (e == NULL || req == NULL) {
        return NULL;
    }
    if (cmd_job_find(e, req->id) != NULL) {
        return NULL;
    }
    if (e->job_count >= PROTO_MAX_PENDING) {
        e->pending_full++;
        return NULL;
    }

    struct cmd_job *job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return NULL;
    }
    job->engine = e;
    snprintf(job->id, sizeof(job->id), "%s", req->id);
    snprintf(job->cmd, sizeof(job->cmd), "%s", req->cmd);
    job->active = 1;
    job->seq = 0;
    job->user_data = user_data;
    job->on_cancel = on_cancel;
    if (cmd_job_slot_add(e, job) != 0) {
        free(job);
        e->pending_full++;
        return NULL;
    }
    return job;
}

static void cmd_job_destroy(struct cmd_job *job)
{
    if (job == NULL) {
        return;
    }
    if (job->engine != NULL) {
        cmd_job_slot_remove(job->engine, job);
    }
    job->active = 0;
    free(job);
}

void cmd_job_cancel(struct cmd_job *job)
{
    if (job == NULL || !job->active) {
        return;
    }
    if (job->on_cancel != NULL) {
        job->on_cancel(job);
    }
    cmd_job_destroy(job);
}

int cmd_job_reply_partial(struct cmd_job *job, cJSON *data)
{
    if (job == NULL || !job->active || job->engine == NULL) {
        cJSON_Delete(data);
        return -1;
    }
    job->seq++;
    cJSON *o = cmd_make_reply_json(job->id, "partial", 1);
    if (o == NULL) {
        cJSON_Delete(data);
        return -1;
    }
    cJSON_AddNumberToObject(o, "seq", job->seq);
    if (data != NULL) {
        cJSON_AddItemToObject(o, "data", data);
    } else {
        cJSON_AddObjectToObject(o, "data");
    }
    return cmd_engine_send_json(job->engine, o);
}

int cmd_job_reply_result(struct cmd_job *job, cJSON *data)
{
    if (job == NULL || !job->active || job->engine == NULL) {
        cJSON_Delete(data);
        return -1;
    }
    cJSON *o = cmd_make_reply_json(job->id, "result", 1);
    if (o == NULL) {
        cJSON_Delete(data);
        return -1;
    }
    cJSON_AddBoolToObject(o, "final", 1);
    if (data != NULL) {
        cJSON_AddItemToObject(o, "data", data);
    } else {
        cJSON_AddObjectToObject(o, "data");
    }
    int rc = cmd_engine_send_json(job->engine, o);
    cmd_job_destroy(job);
    return rc;
}

int cmd_job_reply_error(struct cmd_job *job, const char *code, const char *msg)
{
    if (job == NULL || !job->active || job->engine == NULL) {
        return -1;
    }
    int rc = cmd_engine_send_error(job->engine, job->id, code, msg);
    if (job->on_cancel != NULL) {
        job->on_cancel(job);
    }
    cmd_job_destroy(job);
    return rc;
}

static const cmd_handler_t *cmd_engine_find_handler(cmd_engine_t *e, const char *name)
{
    for (size_t i = 0; i < e->handler_count; i++) {
        if (strcmp(e->handlers[i].name, name) == 0) {
            return &e->handlers[i];
        }
    }
    return NULL;
}

static cmd_mode_t cmd_parse_mode(cJSON *root, int *ok)
{
    *ok = 1;
    cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (m == NULL) {
        return CMD_MODE_SYNC;
    }
    if (!cJSON_IsString(m) || m->valuestring == NULL) {
        *ok = 0;
        return CMD_MODE_SYNC;
    }
    if (strcmp(m->valuestring, "sync") == 0) {
        return CMD_MODE_SYNC;
    }
    if (strcmp(m->valuestring, "async") == 0) {
        return CMD_MODE_ASYNC;
    }
    *ok = 0;
    return CMD_MODE_SYNC;
}

static int cmd_engine_send_ack(cmd_engine_t *e, const char *id)
{
    cJSON *o = cmd_make_reply_json(id, "ack", 1);
    if (o == NULL) {
        return -1;
    }
    return cmd_engine_send_json(e, o);
}

static void cmd_req_init(cmd_req_t *req, cmd_engine_t *e, const char *id, const char *cmd, cmd_mode_t mode, cJSON *root)
{
    memset(req, 0, sizeof(*req));
    req->engine = e;
    snprintf(req->id, sizeof(req->id), "%s", id);
    snprintf(req->cmd, sizeof(req->cmd), "%s", cmd);
    req->mode = mode;
    req->args = cJSON_GetObjectItemCaseSensitive(root, "args");
}

/** sync：handler 当场 reply；若未 reply 则补一条 error */
static void cmd_engine_dispatch_sync(cmd_req_t *req, const cmd_handler_t *h)
{
    if (h->sync_fn == NULL) {
        (void)cmd_req_reply_error(req, "INTERNAL", "sync handler missing");
        return;
    }
    int rc = h->sync_fn(req);
    if (!req->replied) {
        const char *code = (rc != 0) ? "HANDLER_FAIL" : "NO_REPLY";
        const char *msg = (rc != 0) ? "sync handler failed" : "sync handler produced no reply";
        (void)cmd_req_reply_error(req, code, msg);
    }
}

/** async：受理成功后先 ack；后续由 job API 回 partial/result */
static void cmd_engine_dispatch_async(cmd_req_t *req, const cmd_handler_t *h)
{
    if (h->async_fn == NULL) {
        (void)cmd_req_reply_error(req, "INTERNAL", "async handler missing");
        return;
    }
    if (req->engine->job_count >= PROTO_MAX_PENDING) {
        req->engine->pending_full++;
        (void)cmd_req_reply_error(req, "BUSY", "too many pending async commands");
        return;
    }

    struct cmd_job *job = NULL;
    if (h->async_fn(req, &job) != 0 || job == NULL) {
        if (job != NULL) {
            cmd_job_cancel(job);
        }
        (void)cmd_req_reply_error(req, "HANDLER_FAIL", "async handler rejected");
        return;
    }
    (void)cmd_engine_send_ack(req->engine, req->id);
}

void cmd_engine_on_line(session_t *s, const char *line, void *user_data)
{
    (void)s;
    cmd_engine_t *e = user_data;
    if (e == NULL || line == NULL) {
        return;
    }

    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        e->bad_json++;
        fprintf(stderr, "[cmd] bad json: %.80s\n", line);
        (void)cmd_engine_send_error(e, "", "BAD_JSON", "invalid json line");
        return;
    }

    /* ---- 校验请求 ---- */
    cJSON *jid = cJSON_GetObjectItemCaseSensitive(root, "id");
    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(jid) || jid->valuestring == NULL || jid->valuestring[0] == '\0' ||
        !cJSON_IsString(jcmd) || jcmd->valuestring == NULL || jcmd->valuestring[0] == '\0') {
        e->bad_request++;
        (void)cmd_engine_send_error(e, "", "BAD_REQUEST", "missing id or cmd");
        goto out;
    }

    const char *id = jid->valuestring;
    const char *cmd = jcmd->valuestring;

    int mode_ok = 1;
    cmd_mode_t mode = cmd_parse_mode(root, &mode_ok);
    if (!mode_ok) {
        e->bad_request++;
        (void)cmd_engine_send_error(e, id, "BAD_MODE", "mode must be sync|async");
        goto out;
    }

    const cmd_handler_t *h = cmd_engine_find_handler(e, cmd);
    if (h == NULL) {
        e->unknown_cmd++;
        (void)cmd_engine_send_error(e, id, "UNKNOWN_CMD", cmd);
        goto out;
    }
    if (h->mode != mode) {
        e->mode_mismatch++;
        const char *hint = (h->mode == CMD_MODE_ASYNC) ? "command requires mode=async" : "command requires mode=sync";
        (void)cmd_engine_send_error(e, id, "BAD_MODE", hint);
        goto out;
    }
    if (mode == CMD_MODE_ASYNC && cmd_job_find(e, id) != NULL) {
        e->bad_request++;
        (void)cmd_engine_send_error(e, id, "DUPLICATE_ID", "async id already pending");
        goto out;
    }

    /* ---- 分发 ---- */
    cmd_req_t req;
    cmd_req_init(&req, e, id, cmd, mode, root);
    if (mode == CMD_MODE_SYNC) {
        cmd_engine_dispatch_sync(&req, h);
    } else {
        cmd_engine_dispatch_async(&req, h);
    }

out:
    cJSON_Delete(root);
}

void cmd_engine_on_offline(session_t *s, void *user_data)
{
    (void)s;
    cmd_engine_t *e = user_data;
    if (e == NULL) {
        return;
    }
    printf("[cmd] offline → cancel %d async job(s)\n", e->job_count);
    while (e->job_count > 0) {
        struct cmd_job *job = e->jobs[0];
        cmd_job_cancel(job);
    }
}

int cmd_engine_init(cmd_engine_t *e, session_t *session, ev_loop_t *loop, const cmd_handler_t *handlers, size_t handler_count)
{
    if (e == NULL || session == NULL || loop == NULL || handlers == NULL) {
        return -1;
    }
    memset(e, 0, sizeof(*e));
    e->session = session;
    e->loop = loop;
    e->handlers = handlers;
    e->handler_count = handler_count;
    return 0;
}

void cmd_engine_deinit(cmd_engine_t *e)
{
    if (e == NULL) {
        return;
    }
    cmd_engine_on_offline(NULL, e);
}
