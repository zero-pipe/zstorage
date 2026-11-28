#ifndef ZCS_SERVER_H
#define ZCS_SERVER_H

/**
 * @file server.h
 * @brief ZCS 媒体服务器接口（Stable SDK 主入口之一）。
 *
 * 嵌入推荐：#include "zcs/sdk.h"（本头由 sdk.h 包含）。
 * 分层与指南：docs/stream-pipeline.md
 *
 * 典型用法：
 *   zcs_server *s = zcs_server_create_from_ini("config.ini");
 *   zcs_server_set_hooks(s, &hooks);
 *   zcs_server_start(s);
 *   zcs_server_run(s, -1);   // 阻塞直到 Ctrl+C / request_stop
 *   zcs_server_stop(s);
 *   zcs_server_destroy(s);
 *
 * 业务侧优先只依赖 sdk.h / 本头；协议与 GOP 等头属 Advanced/Internal。
 */
#include "zcs/core/event.h"
#include "zcs/ops/config/config.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_server zcs_server;

/** 与 zcs_media_events 对齐的生命周期回调（可只填需要的字段）。 */
typedef zcs_media_events zcs_server_hooks;

/**
 * 从 INI 创建服务器实例（未 start）。
 * @param config_path  可为 NULL（使用内置默认配置）
 * @param log_override 非空时覆盖 [general] log_file
 */
ZCS_API zcs_server *zcs_server_create_from_ini(const char *config_path, const char *log_override);

/** 使用已填充的 zcs_config 创建（调用方保留 cfg 所有权；内部会拷贝一份）。 */
ZCS_API zcs_server *zcs_server_create(const zcs_config *cfg);

/** 销毁服务器：若仍 running 先停服务、关闭运行时，再释放对象。对 NULL 安全。 */
ZCS_API void zcs_server_destroy(zcs_server *s);

/** 在 start 前设置；传 NULL 清除。返回 0 成功。 */
ZCS_API int zcs_server_set_hooks(zcs_server *s, const zcs_server_hooks *hooks);

/** 启动监听（RTMP/HTTP/RTSP/SRT 等按配置与编译选项）。 */
ZCS_API zn_err_t zcs_server_start(zcs_server *s);

/** 停止协议服务（可再次 start 前需 destroy 重建；当前实现为一次性生命周期）。 */
ZCS_API void zcs_server_stop(zcs_server *s);

/**
 * 阻塞运行直到 zcs_server_request_stop() 或默认信号（若已 install）。
 * @param seconds  <0 一直跑；>=0 最多跑 seconds 秒（仍可被 stop 打断）
 */
ZCS_API void zcs_server_run(zcs_server *s, int seconds);

/** 请求 run() 返回（线程安全：仅置标志）。 */
ZCS_API void zcs_server_request_stop(zcs_server *s);

/** 安装 Ctrl+C / SIGTERM 等，内部调用 request_stop。可在 create 后、run 前调用。 */
ZCS_API void zcs_server_install_default_signals(zcs_server *s);

/** 只读配置（start 前后均可；勿长期保存指针跨 destroy）。 */
ZCS_API const zcs_config *zcs_server_config(const zcs_server *s);

/** 是否已 start。 */
ZCS_API int zcs_server_is_running(const zcs_server *s);

/**
 * 向 stdout 打印推流/播放 URL 提示（demo / 联调用）。
 * start 成功后调用。
 */
ZCS_API void zcs_server_print_endpoints(const zcs_server *s);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SERVER_H */
