/**
 * @file    net_buf.c
 */
#include "net_buf.h"

#include <stdlib.h>
#include <string.h>

int net_buf_init(net_buf_t *buf, size_t capacity)
{
    if (buf == NULL || capacity == 0) {
        return -1;
    }
    buf->data = calloc(1, capacity);
    if (buf->data == NULL) {
        return -1;
    }
    buf->capacity = capacity;
    buf->len = 0;
    return 0;
}

void net_buf_deinit(net_buf_t *buf)
{
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->capacity = 0;
    buf->len = 0;
}

void net_buf_clear(net_buf_t *buf)
{
    if (buf != NULL) {
        buf->len = 0;
    }
}

size_t net_buf_space(const net_buf_t *buf)
{
    return (buf == NULL) ? 0 : (buf->capacity - buf->len);
}

size_t net_buf_len(const net_buf_t *buf)
{
    return (buf == NULL) ? 0 : buf->len;
}

int net_buf_append(net_buf_t *buf, const void *data, size_t n)
{
    if (buf == NULL || (n > 0 && data == NULL)) {
        return -1;
    }
    if (n > net_buf_space(buf)) {
        return -1;
    }
    if (n > 0) {
        memcpy(buf->data + buf->len, data, n);
        buf->len += n;
    }
    return 0;
}

void net_buf_consume(net_buf_t *buf, size_t n)
{
    if (buf == NULL || n == 0) {
        return;
    }
    if (n >= buf->len) {
        buf->len = 0;
        return;
    }
    memmove(buf->data, buf->data + n, buf->len - n);
    buf->len -= n;
}

int net_buf_take_line(net_buf_t *buf, char *out, size_t out_cap, size_t max_line)
{
    if (buf == NULL || out == NULL || out_cap < 2) {
        return -1;
    }

    size_t i = 0;
    while (i < buf->len && buf->data[i] != '\n') {
        i++;
        if (i > max_line) {
            return -1; /* 行过长 */
        }
    }
    if (i >= buf->len) {
        if (buf->len > max_line) {
            return -1;
        }
        return 0; /* 未凑齐 */
    }

    /* 一行：data[0..i-1]，可选 \\r */
    size_t line_len = i;
    if (line_len > 0 && buf->data[line_len - 1] == '\r') {
        line_len--;
    }
    if (line_len + 1 > out_cap) {
        return -1;
    }
    memcpy(out, buf->data, line_len);
    out[line_len] = '\0';
    net_buf_consume(buf, i + 1); /* 含 \\n */
    return 1;
}
