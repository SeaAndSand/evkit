/**
 * @file    proto.h
 * @brief   NDJSON 命令协议（一行一个 JSON 对象，以 \\n 结束）
 *
 * 服务端 → 客户端（请求）:
 *   {"id":"r1","cmd":"ping","mode":"sync"}
 *   {"id":"r2","cmd":"echo","mode":"sync","args":{"text":"hi"}}
 *   {"id":"r3","cmd":"count","mode":"async","args":{"n":5,"interval_ms":100}}
 *
 * mode: "sync" | "async"（缺省 sync）
 *   sync  — 在本轮处理完并回一条 result/error
 *   async — 先回 ack，再回 0..N 条 partial，最后一条 result（final）
 *
 * 客户端 → 服务端（应答）:
 *   {"id":"r1","type":"result","ok":true,"data":{...}}
 *   {"id":"r1","type":"error","ok":false,"error":{"code":"...","message":"..."}}
 *   {"id":"r3","type":"ack","ok":true}
 *   {"id":"r3","type":"partial","ok":true,"seq":1,"data":{...}}
 *   {"id":"r3","type":"result","ok":true,"final":true,"data":{...}}
 *
 * 异常约定:
 *   - 缺 id/cmd、JSON 非法、行过长 → 尽量回 PROTOCOL 错误；无法关联 id 则丢弃并计数
 *   - 未知 cmd → error UNKNOWN_CMD
 *   - sync 命令却声明需异步 → error BAD_MODE
 *   - 断线 → 取消所有未完成 async，本端不再发送（对端已不可达）
 */
#ifndef TCP_CMD_PROTO_H
#define TCP_CMD_PROTO_H

#define PROTO_MAX_LINE       (64 * 1024)
#define PROTO_MAX_ID_LEN     64
#define PROTO_MAX_CMD_LEN    64
#define PROTO_MAX_PENDING    16
#define PROTO_TX_CAPACITY    (256 * 1024)
#define PROTO_RX_CAPACITY    (PROTO_MAX_LINE + 1024)

#define PROTO_CONNECT_TIMEOUT_MS  3000u
#define PROTO_RECONNECT_MS        1000u

#endif
