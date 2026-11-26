/**
 * @file srtp.h
 * @brief SRTP/SRTCP 包变换。
 */
#ifndef ZMS_MEDIA_WIRE_SRTP_H
#define ZMS_MEDIA_WIRE_SRTP_H

/**
 * WebRTC SRTP/SRTCP 加密壳（RFC 3711 AES-128-CM + HMAC-SHA1 80-bit）。
 *
 * 操作 zms/session/rtp/* 的明文 RTP/RTCP 缓冲；不解析载荷。
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_WEBRTC_SRTP_KEY_LEN 16u
#define ZMS_WEBRTC_SRTP_SALT_LEN 14u
#define ZMS_WEBRTC_SRTP_TAG_LEN 10u

typedef struct zms_webrtc_srtp zms_webrtc_srtp;

/** 创建 SRTP 上下文（尚未配置密钥）。@return 新上下文，内存不足返回 NULL。 */
ZMS_API zms_webrtc_srtp *zms_webrtc_srtp_create(void);
/** 销毁 SRTP 上下文。 */
ZMS_API void zms_webrtc_srtp_destroy(zms_webrtc_srtp *ctx);

/** WHEP 出站 / 播放发送方向。 */
ZMS_API int zms_webrtc_srtp_init_send(zms_webrtc_srtp *ctx, const uint8_t *key,
                                      const uint8_t *salt);
/**
 * 就地加密一个 RTP 包（原地追加 10 字节 auth tag）。
 * @param len_io 入参为明文长度，出参为密文长度；cap 为缓冲总容量
 * @return 0 成功；-1 失败（参数/容量/密钥问题）
 */
ZMS_API int zms_webrtc_srtp_protect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap);

/** WHIP 入站 / 播放接收方向。 */
ZMS_API int zms_webrtc_srtp_init_recv(zms_webrtc_srtp *ctx, const uint8_t *key,
                                      const uint8_t *salt);
/**
 * 就地去密一个 SRTP 包（校验并移除 auth tag）。
 * @param len_io 入参为密文长度，出参为明文长度
 * @return 0 成功；-1 失败（认证失败/参数非法）
 */
ZMS_API int zms_webrtc_srtp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io);

/** 明文 RTCP compound SRTCP（RFC 3711 §4.2）。 */
ZMS_API int zms_webrtc_srtcp_protect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io,
                                     size_t cap);
/**
 * 就地去密一个 SRTCP 包（复合 RTCP）。
 * @param len_io 入参为密文长度，出参为明文长度
 * @return 0 成功；-1 失败（认证失败/参数非法）
 */
ZMS_API int zms_webrtc_srtcp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io);

/** 会话级暂存（SRTP HMAC）；须为 ZMS_WEBRTC_PLAY_CRYPT_BYTES。 */
ZMS_API void zms_webrtc_srtp_bind_scratch(zms_webrtc_srtp *ctx, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_WIRE_SRTP_H */
