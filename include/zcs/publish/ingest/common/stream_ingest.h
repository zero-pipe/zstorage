#ifndef ZCS_PUBLISH_INGEST_COMMON_STREAM_INGEST_H
#define ZCS_PUBLISH_INGEST_COMMON_STREAM_INGEST_H

/**
 * @file stream_ingest.h
 * @brief 直播入站汇聚：归一化时间戳并写入 gop_queue。
 *
 * RTMP、RTSP RECORD、SRT publish、代理拉流等直播入站路径均在此汇聚。
 * 对直播流应用 @ref zcs_media_timeline，将容器载荷解为 ES 后写入 gop_queue。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/stream/hub.h"
#include "zcs/core/frame.h"
#include "zcs/media/container/demux_pipeline.h"
#include "zcs/media/media_limits.h"
#include "zcs/publish/ingest/common/protocol_opts.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;

#define ZCS_LIVE_INGEST_WORK_BUF ZCS_MEDIA_IO_BUF_SIZE
#define ZCS_LIVE_INGEST_SCRATCH_PAD 256u

typedef struct zcs_live_ingest zcs_live_ingest;

/** 以精确 app/stream 创建入站（代理与测试）。 */
ZCS_API zcs_live_ingest *zcs_live_ingest_create(const char *app, const char *stream,
                                                const zcs_protocol_opts *opts);

/** 创建推流入站；服务端可能追加唯一 stream 后缀。 */
ZCS_API zcs_live_ingest *zcs_live_ingest_create_publish(const char *app,
                                                        const char *stream_requested,
                                                        const zcs_protocol_opts *opts);

/** 以显式 schema 创建推流入站（如 ZCS_SCHEMA_SRT）。 */
ZCS_API zcs_live_ingest *zcs_live_ingest_create_publish_schema_vhost(const char *schema,
                                                                     const char *vhost,
                                                                     const char *app,
                                                                     const char *stream_requested,
                                                                     const zcs_protocol_opts *opts);
/** RTMP schema、指定 vhost 的推流入站；stream 名可能被追加唯一后缀。 */
ZCS_API zcs_live_ingest *zcs_live_ingest_create_publish_vhost(const char *vhost, const char *app,
                                                              const char *stream_requested,
                                                              const zcs_protocol_opts *opts);
/**
 * 指定 vhost 的精确 app/stream 入站（默认 vhost 版本见 create）。
 * opts 为 NULL 时使用协议默认选项。
 */
ZCS_API zcs_live_ingest *zcs_live_ingest_create_vhost(const char *vhost, const char *app,
                                                      const char *stream,
                                                      const zcs_protocol_opts *opts);
/** 指定 schema（默认 vhost）的推流入站；schema 为空回退 RTMP。 */
ZCS_API zcs_live_ingest *zcs_live_ingest_create_publish_schema(const char *schema, const char *app,
                                                               const char *stream_requested,
                                                               const zcs_protocol_opts *opts);

/** 绑定到已注册的媒体源。 */
ZCS_API zcs_live_ingest *zcs_live_ingest_bind(zcs_media_source *src);
/** 销毁入站上下文、RTMP demux 与工作缓冲；不注销所绑定的媒体源本身。 */
ZCS_API void zcs_live_ingest_destroy(zcs_live_ingest *in);
/** 绑定会话 poller：工作区 / large / hevc_au 走 poller 本地池（须在首次 input 前调用） */
ZCS_API void zcs_live_ingest_set_poller(zcs_live_ingest *in, struct zn_poller *poller);
/** 取入站绑定的媒体源（借用，不转移所有权）；未绑定返回 NULL。 */
ZCS_API zcs_media_source *zcs_live_ingest_source(zcs_live_ingest *in);
/** 重置入站上游状态并清空媒体源数据（推流重连时使用）。 */
ZCS_API void zcs_live_ingest_reset(zcs_live_ingest *in);
/** 仅重置入站状态（demux/timeline/config）；保留 gop_queue 供活跃读者。 */
ZCS_API void zcs_live_ingest_reset_upstream(zcs_live_ingest *in);

/** 入站 H.264 Annex-B 帧：重算关键帧/配置帧标志后写 gop_queue，按 ES 刷出 SPS/PPS。 */
ZCS_API zn_err_t zcs_live_ingest_input_h264_annexb(zcs_live_ingest *in, const uint8_t *annexb,
                                                    size_t len, uint32_t dts_ms, uint32_t pts_ms,
                                                    int keyframe);

/** 入站 AAC raw ES（首帧若为 ADTS 会自动提取配置）；标记真实音轨并写入 gop_queue。 */
ZCS_API zn_err_t zcs_live_ingest_input_aac_es(zcs_live_ingest *in, const uint8_t *aac, size_t len,
                                               uint32_t dts_ms);

/** 入站 H.265 Annex-B 帧：语义同 H.264 版本，配置集为 VPS+SPS+PPS。 */
ZCS_API zn_err_t zcs_live_ingest_input_h265_annexb(zcs_live_ingest *in, const uint8_t *annexb,
                                                    size_t len, uint32_t dts_ms, uint32_t pts_ms,
                                                    int keyframe);

/** 入站 G.711（A-law/μ-law）音频 ES；codec 区分 ALAW 与 MULAW。 */
ZCS_API zn_err_t zcs_live_ingest_input_g711_es(zcs_live_ingest *in, zcs_codec_id codec,
                                                const uint8_t *g711, size_t len, uint32_t dts_ms);

/** 预构建 @ref zcs_frame 的统一入站入口。 */
ZCS_API zn_err_t zcs_live_ingest_input_frame(zcs_live_ingest *in, const zcs_frame *frame);

/** 入站 RTMP 视频 tag 体：经内部 FLV-tag demux 解为 ES 后写入。 */
ZCS_API zn_err_t zcs_live_ingest_input_rtmp_video(zcs_live_ingest *in, uint32_t tag_dts_ms,
                                                   const void *body, size_t body_len);

/** 入站 RTMP 音频 tag 体：经内部 FLV-tag demux 解为 ES 后写入。 */
ZCS_API zn_err_t zcs_live_ingest_input_rtmp_audio(zcs_live_ingest *in, uint32_t tag_dts_ms,
                                                   const void *body, size_t body_len);

/** RTMP demux 管线；协议层拥有，入站借用。 */
ZCS_API zcs_demux_pipeline *zcs_live_ingest_rtmp_demux_create(zcs_live_ingest *in);
/** 解除对 RTMP demux 的借用（仅置空指针，不销毁管线——所有权在协议层）。 */
ZCS_API void zcs_live_ingest_rtmp_demux_release(zcs_live_ingest *in);

/**
 * 由十六进制 AudioSpecificConfig 设置 AAC 音频配置并标记音轨。
 * @param config_hex 形如 "1210" 的 ASC 十六进制串
 * @return ZN_OK 成功；解析/构造失败返回错误。
 */
ZCS_API zn_err_t zcs_live_ingest_set_aac_config_hex(zcs_live_ingest *in, const char *config_hex);
/** 标记收到真实音轨（用于关闭静音补轨）。 */
ZCS_API void zcs_live_ingest_mark_real_audio(zcs_live_ingest *in);
/**
 * 若尚无音频配置，则按给定采样率/声道构造默认 AAC 配置；已有配置时直接返回 ZN_OK。
 */
ZCS_API zn_err_t zcs_live_ingest_ensure_aac_config(zcs_live_ingest *in, int sample_rate,
                                                    int channels);
/** 设置音频编解码器与采样率（用于 RTSP/SDP 场景，供时间线计算）。 */
ZCS_API void zcs_live_ingest_set_audio_codec(zcs_live_ingest *in, zcs_codec_id codec,
                                             uint32_t sample_rate);
/**
 * 设置 RTP 时钟率（视频 clock 与可选音频 clock），并复位上次时间戳。
 * 用于 RTSP 入站：把 RTP 时间戳换算为毫秒时间线。
 */
ZCS_API void zcs_live_ingest_set_rtp_clocks(zcs_live_ingest *in, uint32_t video_clock_hz,
                                            zcs_codec_id audio_codec, uint32_t audio_clock_hz);
/** 放宽 stamp_revise 增量钳制（默认 300ms）。 */
ZCS_API void zcs_live_ingest_set_stamp_max_delta(zcs_live_ingest *in, uint32_t max_delta_ms);
/** MPEG-TS/SRT：PES 音/视频 PTS 原点不同；关闭 300ms A/V 钳制（默认开启）。 */
ZCS_API void zcs_live_ingest_set_stamp_av_clamp(zcs_live_ingest *in, int enabled);
/** MPEG-TS/SRT：demux PTS/DTS 毫秒直接写入 gop_queue（共用 PES 时间轴）。 */
ZCS_API void zcs_live_ingest_set_timeline_linear_ms(zcs_live_ingest *in, int on);
/** MPEG-TS/SRT：首个 IDR 前延迟 gop_queue video_config（避免 IDR 前 GOP 缓存）。 */
ZCS_API void zcs_live_ingest_set_defer_gop_vcfg(zcs_live_ingest *in, int on);
/**
 * 设置 H.264 的 SPS/PPS 并刷新视频配置。
 * 仅在缺失配置或分辨率/帧率更优时才更新，避免占位 SPS 反复覆盖真实配置。
 * @return ZN_OK（含“无需更新”情况）；参数非法或构造失败返回错误。
 */
ZCS_API zn_err_t zcs_live_ingest_set_h264_sps_pps(zcs_live_ingest *in, const uint8_t *sps,
                                                   size_t sps_len, const uint8_t *pps,
                                                   size_t pps_len);
/** 设置 H.265 的 VPS/SPS/PPS 并刷新视频配置；语义同 H.264 版本。 */
ZCS_API zn_err_t zcs_live_ingest_set_h265_vps_sps_pps(zcs_live_ingest *in, const uint8_t *vps,
                                                       size_t vps_len, const uint8_t *sps,
                                                       size_t sps_len, const uint8_t *pps,
                                                       size_t pps_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PUBLISH_INGEST_COMMON_STREAM_INGEST_H */
