/**
 * @file sdp_internal.h
 * @brief WebRTC SDP 处理的跨文件共享助手（仅 session/webrtc 内部）。
 *
 * sdp 拆为三个文件：
 *   sdp.c        核心助手（编码解析、凭据、ICE 解析、fmtp、协商）+ 对外入口
 *   sdp_answer.c 应答 SDP 文本构造（video/audio/media 段、bundle、候选、TWCC）
 *   sdp_client.c 主动连接：生成 offer、应用 answer、probe
 */
#ifndef ZCS_SRC_SESSION_WEBRTC_SDP_INTERNAL_H
#define ZCS_SRC_SESSION_WEBRTC_SDP_INTERNAL_H

#include "session/webrtc/session_internal.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/protocol/rtsp/sdp.h"
#include <stddef.h>
#include <stdint.h>

/** 协商得到的单轨参数。 */
typedef struct webrtc_neg_track {
    zcs_codec_id codec;
    uint8_t pt;
    int rate;
    int channels;
} webrtc_neg_track;

/** 格式化追加一行 SDP，返回新的写入长度（越界返回原 n）。 */
int webrtc_sdp_append(char *answer, size_t answer_cap, size_t n, const char *fmt, ...);

/** 生成随机 ICE 凭据（字母表，长度 want）。 */
void webrtc_rand_credential(char *out, size_t cap, size_t want);

/** 从 offer/answer 解析远端 ICE ufrag/pwd，成功返回 0。 */
int webrtc_parse_remote_ice(sdp_t *sdp, char *ufrag, size_t ufrag_cap, char *pwd, size_t pwd_cap);

/** 首个指定类型媒体段的索引；无则 -1。 */
int webrtc_media_index(sdp_t *sdp, const char *media_type);

/** 编码名 → zcs_codec_id（未知返回 ZCS_CODEC_INVALID）。 */
zcs_codec_id webrtc_codec_from_encoding(const char *encoding);

/** 从源/回放读取 H.264 profile 与 fmtp 参数。 */
void webrtc_h264_load_fmtp(const zcs_webrtc_session *s, char *profile, size_t profile_cap,
                           char *sprop, size_t sprop_cap);

/** 协商 offer 到会话：写 PT/mid/编解码/发布布局。成功返回 0。 */
int webrtc_session_negotiate(zcs_webrtc_session *mut, sdp_t *sdp, int for_play);

/** 读 offer 的 a=setup，返回 1 表示 passive。 */
int webrtc_offer_setup_passive(sdp_t *sdp);

/** WHIP：把 offer 的 H.264 sprop-parameter-sets 存进会话（发布用）。 */
int webrtc_publish_store_offer_h264_sprop(zcs_webrtc_session *s, sdp_t *sdp);

#endif /* ZCS_SRC_SESSION_WEBRTC_SDP_INTERNAL_H */
