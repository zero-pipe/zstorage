/**
 * @file session_auth.h
 * @brief RTSP 鉴权。
 */
#ifndef ZCS_SESSION_RTSP_SESSION_AUTH_H
#define ZCS_SESSION_RTSP_SESSION_AUTH_H
#include "zcs/protocol/rtsp/parser.h"
#include "zcs/zcs_export.h"
#ifdef __cplusplus
extern "C" {
#endif
struct zcs_rtsp_session;
/** 配置 RTSP Digest 鉴权；user/pass 均为空则关闭 */
ZCS_API void zcs_rtsp_auth_configure(const char *user, const char *pass);
/** 测试用：PLAY 拉流 SETUP 拒绝 TCP interleaved 返回 461，用于验证客户端 AUTO 回退 UDP */
ZCS_API void zcs_rtsp_set_test_reject_tcp_setup(int enable);
/** 是否已配置 Digest 鉴权（user 与 pass 均非空）。 */
ZCS_API int zcs_rtsp_auth_enabled(void);
/** @return 1 可继续处理请求；0 已回 401 */
ZCS_API int zcs_rtsp_auth_check(struct zcs_rtsp_session *rs, const zcs_rtsp_message *msg);
/** 查询测试开关：是否对所有 SETUP 拒绝 TCP interleaved 并回 461。 */
ZCS_API int zcs_rtsp_test_reject_tcp_setup(void);
/** RTSP 传输方式策略：-1 不限 / 0 仅 TCP / 1 仅 UDP / 2 MULTICAST（暂不支持） */
ZCS_API void zcs_rtsp_set_rtp_transport_type(int type);
/** 查询当前 RTSP 传输方式策略（-1 表示不限）。 */
ZCS_API int zcs_rtsp_rtp_transport_type(void);
/** RTSP 播放低延迟（不按播放时钟 pacing）。 */
ZCS_API void zcs_rtsp_set_low_latency(int enable);
/** 查询是否启用 RTSP 低延迟模式。 */
ZCS_API int zcs_rtsp_low_latency_enabled(void);
/** RTSP 组播出流配置（port_base=0 关闭） */
ZCS_API void zcs_rtsp_multicast_configure(const char *addr, unsigned port_base, int ttl);
#ifdef __cplusplus
}
#endif
#endif /* ZCS_SESSION_RTSP_SESSION_AUTH_H */
