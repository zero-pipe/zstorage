#ifndef ZCS_EGRESS_RTP_RTP_MUXER_H
#define ZCS_EGRESS_RTP_RTP_MUXER_H

/**
 * @file rtp_muxer.h
 * @brief ES / gop_queue 槽位打成 RTP 包（出站协议规范）。
 *
 * 打包器：src/egress/rtp/rtp_payload_muxer.c（内部用 zm_rtp_writer）。
 * RTSP 会话拥有 TCP interleaved 发送；本模块经回调输出 RTP。
 */
#include "zcs/media/codec/codec_id.h"
#include "zcs/core/gop/queue.h"
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_rtp_mux_track {
    ZCS_RTP_MUX_TRACK_VIDEO = 0,
    ZCS_RTP_MUX_TRACK_AUDIO = 1,
} zcs_rtp_mux_track;

typedef void (*zcs_rtp_mux_on_rtp)(zcs_rtp_mux_track track, const uint8_t *rtp, size_t len,
                                   void *user);

typedef struct zcs_rtp_muxer_stats {
    uint32_t video_pkt_count;
    uint32_t video_octet_count;
    uint32_t audio_pkt_count;
    uint32_t audio_octet_count;
    uint32_t video_last_rtp_ts;
    uint32_t audio_last_rtp_ts;
} zcs_rtp_muxer_stats;

typedef struct zcs_rtp_muxer_opts {
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    uint32_t video_ssrc;
    uint32_t audio_ssrc;
    uint16_t video_seq;
    uint16_t audio_seq;
    int audio_rate;
    zcs_codec_id audio_codec;
    uint8_t video_pt;
    uint8_t audio_pt;
    /** AAC AudioSpecificConfig（非 FLV seq header）；mpeg4-generic RTP 必需。 */
    const uint8_t *audio_extra;
    size_t audio_extra_len;
    /** AV1 AV1CodecConfigurationRecord（raw av1c）；librtsp sdp_av1 / RTP payload 初始化必需。 */
    const uint8_t *video_extra;
    size_t video_extra_len;
} zcs_rtp_muxer_opts;

typedef struct zcs_rtp_muxer zcs_rtp_muxer;

/**
 * 创建 RTP 打包器。opts 内容会被复制，其中的 audio_extra/video_extra 会另行拷贝缓存，
 * 调用返回后 opts 及其缓冲即可释放，打包器不再借用。
 * @param opts   初始化参数；seq 为 0 时按 1 起步。
 * @param on_rtp 每生成一个 RTP 包的回调，不得为 NULL。
 * @param user   透传给 on_rtp 的上下文。
 * @return 打包器实例；参数非法或分配失败返回 NULL。用 zcs_rtp_muxer_destroy 释放。
 */
ZCS_API zcs_rtp_muxer *zcs_rtp_muxer_create(const zcs_rtp_muxer_opts *opts,
                                            zcs_rtp_mux_on_rtp on_rtp, void *user);
/** 销毁打包器并释放内部 codec 缓存；对 NULL 安全。 */
ZCS_API void zcs_rtp_muxer_destroy(zcs_rtp_muxer *m);
/** 重置统计、seq、播放时钟与缓存，可复用同一实例开启新一轮会话。 */
ZCS_API void zcs_rtp_muxer_reset(zcs_rtp_muxer *m);

/** 记录墙钟 NTP 并重置出站 epoch（每 PLAY 会话一次）。 */
ZCS_API void zcs_rtp_muxer_arm_play(zcs_rtp_muxer *m);

/** 跟随直播边缘：解锁 epoch 直至下次 IDR 重锁（配合 seek_live）。 */
ZCS_API void zcs_rtp_muxer_jump_live(zcs_rtp_muxer *m);

/** 取打包统计（借用指针，随实例销毁失效）；m 为 NULL 时返回 NULL。 */
ZCS_API const zcs_rtp_muxer_stats *zcs_rtp_muxer_get_stats(const zcs_rtp_muxer *m);
/** 取只读播放时钟（借用指针，随实例销毁失效）。 */
ZCS_API const zcs_egress_clock *zcs_rtp_muxer_play_clock(const zcs_rtp_muxer *m);
/** 可变播放时钟，供 pacing 辅助（如 zcs_egress_clock_sync_wall）。 */
ZCS_API zcs_egress_clock *zcs_rtp_muxer_play_clock_mut(zcs_rtp_muxer *m);
/** 下一个待发送的视频 RTP 序列号（用于 RTSP RTP-Info）。 */
ZCS_API uint16_t zcs_rtp_muxer_video_seq(const zcs_rtp_muxer *m);
/** 下一个待发送的音频 RTP 序列号（用于 RTSP RTP-Info）。 */
ZCS_API uint16_t zcs_rtp_muxer_audio_seq(const zcs_rtp_muxer *m);

/** 主输入：一条交织 ring 槽位 RTP。 */
ZCS_API int zcs_rtp_muxer_input_slot(zcs_rtp_muxer *m, const zcs_gop_slot *slot);

/** 遗留 RTMP tag 输入（迁移 / 测试）。 */
ZCS_API int zcs_rtp_muxer_input_rtmp(zcs_rtp_muxer *m, uint8_t type_id, uint32_t tag_dts_ms,
                                     const uint8_t *data, size_t len, uint8_t *scratch,
                                     size_t scratch_cap);

/** VOD seek 突发：预算耗尽前跳过实时 pacing。 */
ZCS_API void zcs_rtp_muxer_set_catchup(zcs_rtp_muxer *m, int on);
/**
 * 设置追帧预算：on 非零时最多跳过 max_frames 帧的实时 pacing，之后自动退出追帧；
 * on 为 0 时关闭并清零计数。max_frames<=0 取默认 256。
 */
ZCS_API void zcs_rtp_muxer_set_catchup_budget(zcs_rtp_muxer *m, int on, int max_frames);
/** 查询是否仍处于追帧突发（预算未耗尽）。 */
ZCS_API int zcs_rtp_muxer_catchup_on(const zcs_rtp_muxer *m);
/** 告知已处理一帧追帧并递减预算，耗尽后自动退出追帧。 */
ZCS_API void zcs_rtp_muxer_catchup_frame(zcs_rtp_muxer *m);

/** @return 非零表示尚未发送首个视频关键帧（VOD bootstrap / seek）。 */
ZCS_API int zcs_rtp_muxer_awaiting_video_key(const zcs_rtp_muxer *m);

/** VOD seek：在 seek_ms 锚定绝对 RTP 时间线（RFC 2326 RTP-Info）。 */
ZCS_API void zcs_rtp_muxer_begin_vod_seek(zcs_rtp_muxer *m, uint32_t seek_ms);
/** 取当前 VOD seek 的绝对锚点毫秒（RTP-Info 的 rtptime 基准）；未 seek/非 VOD 返回 0。 */
ZCS_API uint32_t zcs_rtp_muxer_vod_anchor_ms(const zcs_rtp_muxer *m);
/** 设置播放倍速（RTSP Scale），由播放时钟做节奏缩放。 */
ZCS_API void zcs_rtp_muxer_set_play_scale(zcs_rtp_muxer *m, double scale);
/** 暂停播放时钟（RTSP PAUSE）；恢复请用 zcs_rtp_muxer_resume_play。 */
ZCS_API void zcs_rtp_muxer_pause_play(zcs_rtp_muxer *m);
/** 恢复播放时钟（RTSP PLAY 续播）。 */
ZCS_API void zcs_rtp_muxer_resume_play(zcs_rtp_muxer *m);

/** H.264 avcC SPS/PPS RTP（ts=0；不锁定 epoch）。 */
ZCS_API void zcs_rtp_muxer_send_avc_config(zcs_rtp_muxer *m, const uint8_t *avcc, size_t avcc_len,
                                           uint32_t anchor_ms);

/** H.265 HVCC/FLV seq header VPS/SPS/PPS RTP（ts=0）。 */
ZCS_API void zcs_rtp_muxer_send_hevc_config(zcs_rtp_muxer *m, const uint8_t *video_cfg,
                                            size_t cfg_len, uint32_t anchor_ms);

/** 由 AV1 FLV 序列头 / 原始 av1C 做 librtsp AV1 RTP 载荷起播。 */
ZCS_API void zcs_rtp_muxer_send_av1_config(zcs_rtp_muxer *m, const uint8_t *video_cfg,
                                           size_t cfg_len, uint32_t anchor_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_RTP_RTP_MUXER_H */
