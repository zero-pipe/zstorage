/**
 * @file device_command.h
 * @brief 设备协议命令表：ONVIF/GB28181 共用的命令描述与能力门控。
 *
 * 适配器把线协议解析成 name + 上下文，然后在自己的静态表里查命令；
 * 新增一条命令 = 表项 + exec，不再散落 if/strstr 链。
 */
#ifndef ZMS_DEVICE_CORE_DEVICE_COMMAND_H
#define ZMS_DEVICE_CORE_DEVICE_COMMAND_H

#include "zms/device/core/device_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 命令执行上下文：session/message 由各适配器解释（不透明）。 */
typedef struct zms_device_cmd_ctx {
    void *session;
    const void *message;
    void *extra;
} zms_device_cmd_ctx;

typedef int (*zms_device_cmd_exec)(const zms_device_cmd_ctx *ctx);

typedef struct zms_device_cmd {
    const char *name;
    /** query / control / action / notify / response（仅用于文档/日志） */
    const char *direction;
    /** 需要的设备能力（ZMS_DEV_CAP_*）；0 = 始终支持 */
    unsigned capability;
    zms_device_cmd_exec exec;
} zms_device_cmd;

/** 大小写不敏感按 name 查表；找不到返回 NULL。 */
const zms_device_cmd *zms_device_cmd_find(const zms_device_cmd *table, size_t count,
                                          const char *name);

/** 能力门控：capability=0 或任一通道具备该能力时允许。 */
int zms_device_cmd_allowed(const zms_device_cmd *cmd);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_DEVICE_CORE_DEVICE_COMMAND_H */
