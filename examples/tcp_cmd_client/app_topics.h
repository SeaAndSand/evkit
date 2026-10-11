/**
 * @file    app_topics.h
 * @brief   tcp_cmd_client 应用层 bus topic（uint32 + ev_bus_match_u32）
 *
 * session 不依赖 bus；由 main 在 online/offline 回调里 publish。
 */
#ifndef TCP_CMD_APP_TOPICS_H
#define TCP_CMD_APP_TOPICS_H

#include <stdint.h>

enum {
    APP_TOPIC_SESSION_ONLINE = 1,
    APP_TOPIC_SESSION_OFFLINE = 2
};

#endif
