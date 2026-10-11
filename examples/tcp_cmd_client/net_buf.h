/**
 * @file    net_buf.h
 * @brief   固定容量线性缓冲：RX 拼包 / TX 待发队列
 */
#ifndef TCP_CMD_NET_BUF_H
#define TCP_CMD_NET_BUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t capacity;
    size_t len;   /* 已用字节 */
} net_buf_t;

int net_buf_init(net_buf_t *buf, size_t capacity);
void net_buf_deinit(net_buf_t *buf);
void net_buf_clear(net_buf_t *buf);

size_t net_buf_space(const net_buf_t *buf);
size_t net_buf_len(const net_buf_t *buf);

/** 追加；空间不足返回 -1 */
int net_buf_append(net_buf_t *buf, const void *data, size_t n);

/** 消费头部 n 字节（TX 已写出 / RX 已解析） */
void net_buf_consume(net_buf_t *buf, size_t n);

/**
 * 从 RX 缓冲取出一行（不含 \\n），写入 out（保证 NUL 结尾）。
 * @return 1 取到一行；0 暂无完整行；-1 行超 max_line（调用方应断开）
 */
int net_buf_take_line(net_buf_t *buf, char *out, size_t out_cap, size_t max_line);

#endif
