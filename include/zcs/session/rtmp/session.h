/**
 * @file session.h
 * @brief RTMP 会话。
 */
#ifndef ZCS_SESSION_RTMP_SESSION_H
#define ZCS_SESSION_RTMP_SESSION_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/net/tcp_server.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_rtmp_session zcs_rtmp_session;

typedef struct zcs_rtmp_session_opts {
    zn_tcp_session *tcp;
} zcs_rtmp_session_opts;

/**
 * 创建 RTMP 会话并绑定已建好的 TCP 连接。
 * @param opts->tcp 必填，会话仅借用该连接、不接管其生命周期。
 * @return 会话实例；opts/tcp 为空或分配失败返回 NULL。用 destroy 释放。
 */
ZCS_API zcs_rtmp_session *zcs_rtmp_session_create(const zcs_rtmp_session_opts *opts);
/** 释放会话（拆除协议/ingest、归还缓冲、销毁互斥量）；对 NULL 安全。 */
ZCS_API void zcs_rtmp_session_destroy(zcs_rtmp_session *s);
/**
 * 投递 TCP 收到的字节到 RTMP 分块/握手解析，按需回发握手与 server-init 数据；
 * 进入 PLAYING 后触发一次 flush。会话已处于销毁流程时调用无操作、不持锁阻塞。
 */
ZCS_API void zcs_rtmp_session_on_recv(zcs_rtmp_session *s, const void *data, size_t len);
/** TCP 错误回调：等价于 schedule_destroy(s, NULL)，安排异步销毁。 */
ZCS_API void zcs_rtmp_session_on_error(zcs_rtmp_session *s);
/**
 * 安排异步销毁：先 teardown 并断开对 TCP 会话的引用，再向 poller 投递销毁任务完成释放。
 * 幂等，重复调用只生效一次；不得再与 destroy 叠加以免双重释放。
 * @param session 仅为异步任务保留的关联 TCP 会话，可为 NULL。
 */
ZCS_API void zcs_rtmp_session_schedule_destroy(zcs_rtmp_session *s, zn_tcp_session *session);
void zcs_rtmp_session_on_manager(zcs_rtmp_session *s);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SESSION_RTMP_SESSION_H */
