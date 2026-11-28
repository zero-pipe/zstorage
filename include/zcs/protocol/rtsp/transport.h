/**
 * @file transport.h
 * @brief RTSP Transport 头解析。
 */
#ifndef ZCS_PROTOCOL_RTSP_TRANSPORT_H
#define ZCS_PROTOCOL_RTSP_TRANSPORT_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_rtsp_rtp_mode {
    ZCS_RTSP_RTP_TCP = 0,
    ZCS_RTSP_RTP_UDP = 1,
    /** 拉流客户端：TCP interleaved，SETUP 461 时回退 UDP（见 rtsp_client.c） */
    ZCS_RTSP_RTP_AUTO = 2,
} zcs_rtsp_rtp_mode;

/** 根据 SETUP Transport 头判断模式；无法识别时默认 TCP */
ZCS_API zcs_rtsp_rtp_mode zcs_rtsp_transport_parse_mode(const char *transport_hdr);

/** opts.rtp_mode AUTO 时由拉流端协商（默认 TCP，461 回退 UDP）；显式 TCP/UDP 则直接选用 */
ZCS_API zcs_rtsp_rtp_mode zcs_rtsp_transport_resolve_mode(zcs_rtsp_rtp_mode mode);

/** 解析 interleaved=N-M（RTP/RTCP 通道号）。成功返回 0，缺失或格式错误返回 -1。 */
ZCS_API int zcs_rtsp_transport_parse_interleaved(const char *transport, uint8_t *rtp_ch,
                                                 uint8_t *rtcp_ch);
/** 解析 server_port=N-M（服务端 RTP/RTCP 端口）。成功返回 0，缺失/零端口返回 -1。 */
ZCS_API int zcs_rtsp_transport_parse_server_port(const char *transport, uint16_t *rtp_port,
                                                 uint16_t *rtcp_port);
/** 解析 client_port=N-M（客户端 RTP/RTCP 端口）。成功返回 0，缺失/零端口返回 -1。 */
ZCS_API int zcs_rtsp_transport_parse_client_port(const char *transport, uint16_t *rtp_port,
                                                 uint16_t *rtcp_port);
/** 解析 ssrc=XXXXXXXXXXXXXXXX（16 进制）。成功返回 0，缺失/非法返回 -1。 */
ZCS_API int zcs_rtsp_transport_parse_ssrc(const char *transport, uint32_t *ssrc);

typedef void (*zcs_rtsp_udp_on_packet_fn)(void *user, int track_idx, int is_rtcp,
                                          const uint8_t *data, size_t len, const char *peer_ip,
                                          uint16_t peer_port);

/** 进程级：RTP/RTCP 本地端口 会话回调 */
ZCS_API void zcs_rtsp_udp_registry_init(void);
/** 销毁 UDP 注册表与端口池（进程退出时调用）；会清空所有绑定槽。 */
ZCS_API void zcs_rtsp_udp_registry_fini(void);

/**
 * 按本地端口注册收包回调（RTP 或 RTCP 各占一个槽，最多 512 个）。
 * @param local_port 接收端口；0 非法
 * @param track_idx  回调时透传的轨道序号
 * @param is_rtcp    非 0 表示这是 RTCP 端口
 * @return ZN_OK 成功；非法参数或槽位用尽返回错误。
 */
ZCS_API zn_err_t zcs_rtsp_udp_registry_bind(uint16_t local_port, int track_idx, int is_rtcp,
                                             zcs_rtsp_udp_on_packet_fn cb, void *user);

/** 解除某本地端口的回调绑定；未绑定则无操作。 */
ZCS_API void zcs_rtsp_udp_registry_unbind(uint16_t local_port);

/** 测试/调试：按端口分发一包（通常由 udp_client 回调调用） */
ZCS_API void zcs_rtsp_udp_registry_dispatch(uint16_t local_port, const uint8_t *data, size_t len,
                                            const char *peer_ip, uint16_t peer_port);

/** 从全局池分配 RTP/RTCP 端口 */
ZCS_API zn_err_t zcs_rtsp_transport_acquire_ports(uint16_t *rtp_port, uint16_t *rtcp_port);
/** 归还由 acquire_ports 分配的端口对（按 rtp_port 定位）；端口池未初始化则无操作。 */
ZCS_API void zcs_rtsp_transport_release_ports(uint16_t rtp_port);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTSP_TRANSPORT_H */
