/**
 * @file    cmds.h
 * @brief   示例命令表：ping/echo（sync 单返回）、slow（async 单返回）、count（async 多返回）
 */
#ifndef TCP_CMD_CMDS_H
#define TCP_CMD_CMDS_H

#include "cmd_engine.h"

const cmd_handler_t *cmds_handlers(size_t *out_count);

#endif
