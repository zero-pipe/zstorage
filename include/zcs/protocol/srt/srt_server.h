/**
 * @file srt_server.h
 * @brief SRT 服务：监听、会话分发与端口信息。
 */
#ifndef ZCS_PROTOCOL_SRT_SERVER_H
#define ZCS_PROTOCOL_SRT_SERVER_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/poller/poller.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_srt_server zcs_srt_server;

typedef struct zcs_srt_server_opts {
    zn_poller *poller;
    const char *host;
    uint16_t port;
    int timeout_sec;  /**< 对端空闲超时（秒），<=0 用默认 5 */
    int latency_mul;  /**< 延迟倍数，<=0 用默认 4（基础 50ms） */
    int pkt_buf_size; /**< 包缓冲大小，<=0 用默认 8192 */
    const char *passphrase; /**< 空/NULL 表示不加密 */
} zcs_srt_server_opts;

/** 二进制在 ZCS_ENABLE_SRT 且链接 libsrt 时编译则为非零。 */
ZCS_API int zcs_srt_server_available(void);

/**
 * 创建 SRT 服务：初始化 libsrt 运行时并立即创建监听套接字（但尚未接入 poller）。
 * @return 服务；opts/poller 为空、libsrt 初始化或监听失败返回 NULL
 */
ZCS_API zcs_srt_server *zcs_srt_server_create(const zcs_srt_server_opts *opts);
/** 销毁服务：停止并释放服务及其全局 SRT 运行时。 */
ZCS_API void zcs_srt_server_destroy(zcs_srt_server *srv);
/**
 * 启动服务：把监听套接字接入 poller 并开始接受连接。
 * @return ZN_OK；已在运行或 poller 初始化失败返回错误
 */
ZCS_API zn_err_t zcs_srt_server_start(zcs_srt_server *srv);
/** 停止服务并关闭监听套接字；未运行时无操作。 */
ZCS_API void zcs_srt_server_stop(zcs_srt_server *srv);
/** @return 监听端口（opts.port 为 0 时为默认 9000）；srv 为 NULL 返回 0。 */
ZCS_API uint16_t zcs_srt_server_port(const zcs_srt_server *srv);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_SRT_SERVER_H */
