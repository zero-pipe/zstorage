/**
 * @file session_api.h
 * @brief HTTP 会话的对外访问器（协议层/业务层与会话内部状态之间的边界）。
 *
 * 背景：`zcs_http_session` 的内部布局只在 HTTP 会话层（service/response/play）可见。
 * 其它模块（webapi、WebRTC 路由、ONVIF）只需要少量信息，统一走本文件的访问器，
 * 不再包含 `session_internal.h`。这样会话结构可以继续演进而不影响调用方。
 */
#ifndef ZCS_PROTOCOL_HTTP_SESSION_API_H
#define ZCS_PROTOCOL_HTTP_SESSION_API_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;
struct zn_poller_pool;
struct zn_tcp_session;
struct zcs_config;
struct zcs_http_session;
typedef struct zn_poller zn_poller;
typedef struct zn_poller_pool zn_poller_pool;
typedef struct zn_tcp_session zn_tcp_session;
typedef struct zcs_config zcs_config;
typedef struct zcs_http_session zcs_http_session;

/** 会话所属 TCP 连接；会话已关闭时返回 NULL。 */
ZCS_API zn_tcp_session *zcs_http_session_tcp(zcs_http_session *hs);
/** 会话所属 poller；未知时返回 NULL。 */
ZCS_API zn_poller *zcs_http_session_poller(zcs_http_session *hs);
/** 会话所属 poller 池；未知时返回 NULL。 */
ZCS_API zn_poller_pool *zcs_http_session_poller_pool(zcs_http_session *hs);
/** webapi 的 secret；未配置时返回 NULL。 */
ZCS_API const char *zcs_http_session_api_secret(zcs_http_session *hs);
/** 全局配置；未知时返回 NULL。 */
ZCS_API const zcs_config *zcs_http_session_config(zcs_http_session *hs);

/* ── 响应发送（路由与协议回调只需这些） ───────────────────────────────── */

/**
 * 所有发送助手都在会话所属 poller 线程调用；
 * 发送失败/连接已断时内部静默处理，不返回错误。
 */
ZCS_API void zcs_http_response_send_error(zcs_http_session *hs, int code, const char *reason);
/** 发送 application/json 响应；body 不要求 NUL 结尾，按 body_len 发送。 */
ZCS_API void zcs_http_response_send_json(zcs_http_session *hs, int status, const char *body,
                                         size_t body_len);
/** 发送任意类型响应（ctype 为 Content-Type 字符串）；body 按 body_len 原样发送。 */
ZCS_API void zcs_http_response_send_bytes(zcs_http_session *hs, int status, const char *ctype,
                                          const void *body, size_t body_len);
/** 以附件下载方式发送磁盘文件（自动填充 Content-Disposition/长度）。 */
ZCS_API void zcs_http_response_send_download_file(zcs_http_session *hs, const char *path);

/* ── 异步任务与会话生命周期 ────────────────────────────────────────────── */

/** 异步任务持有一段会话生命周期（在会话所属 poller 线程调用）。 */
ZCS_API void zcs_http_session_async_hold(zcs_http_session *hs);
/** 异步任务结束，释放引用；最后一个引用者在会话已关闭时真正销毁。 */
ZCS_API void zcs_http_session_async_release(zcs_http_session *hs);
/** 会话是否仍可用于回写（未关闭且连接还在）。 */
ZCS_API int zcs_http_session_is_live(const zcs_http_session *hs);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_HTTP_SESSION_API_H */
