#ifndef ZCS_MEDIA_CONTAINER_MPEGTS_MPEGTS_MUX_FEED_H
#define ZCS_MEDIA_CONTAINER_MPEGTS_MPEGTS_MUX_FEED_H

/**
 * @file mpegts_mux_feed.h
 * @brief 共享 MPEG-TS mux 喂入辅助（H.264/H.265/AAC → PES/TS）。
 *
 * 供 HLS 分片写入器与连续 SRT/HTTP-TS 出站
 *（egress/mpegts/mpegts_egress.c）使用。拥有 zm libmpeg/mpeg4 接触点。
 */
#include "zcs/zcs_export.h"
#include "zcs/media/container/container_registry.h"
#include "zcs/media/clock.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_codec_param_sets zcs_codec_param_sets;

/** 最大 ADTS 帧（raw AAC 载荷上限 0x1fff + ADTS 头）。所有者应将
 *  ADTS 暂存缓冲至少设为此大小，共享 feed 无需再扩容。 */
#define ZCS_MPEGTS_ADTS_MAX (0x1fffu + 16u)

/**
 * 由 HVCC 记录为 HEVC 流构建 MPEG-TS PMT ES_info 字节：
 * registration descriptor（'HEVC'）+ HEVC video descriptor（0x38）。
 *
 * @return 写入 @a out 的字节数，失败为 -1。
 */
ZCS_API int zcs_mpegts_hevc_esinfo(const uint8_t *hvcc, size_t hvcc_len, uint8_t *out, size_t cap);

/**
 * 基于 zm libmpeg 的连续（非分片）MPEG-TS muxer 后端。实现与
 * 分片 HLS 后端相同的 zcs_container_muxer_ops 契约，但经 cfg.on_segment（duration=0）
 * 输出连续 188 字节 TS 流，供 SRT 出站。未在容器注册表中注册（与分片后端共享
 * MPEG-TS 容器 id）；直接引用。
 */
extern const zcs_container_muxer_ops zcs_container_mpegts_continuous_muxer_ops;

/**
 * 已 mux 访问单元的 sink。各所有者注入输出策略而共享 feed 无需感知：
 * SRT 路径直写连续后端，HLS 路径外包分片边界 flush 与末时间戳记账。
 */
typedef zn_err_t (*zcs_mpegts_mux_feed_sink_fn)(void *user, int stream_type, const void *data,
                                                 size_t len, int64_t pts_ms, int64_t dts_ms,
                                                 int flags);

/**
 * 单次调用视角下的所有者 MPEG-TS codec-feed 状态。共享 feed
 * 借用（不拥有）这些指针；所有者保留后备存储。
 * 分片 HLS 与连续 SRT 路径共用，H.264/H.265/AAC mux 编排集中一处。
 */
typedef struct zcs_mpegts_mux_feed_view {
    zcs_mpegts_mux_feed_sink_fn sink; /**< 必需：接收每条 muxed AU */
    void *sink_user;                  /**< 回传给 sink 的 opaque user */
    zcs_codec_param_sets *params;   /**< 关键帧前置用的 VPS/SPS/PPS */
    zcs_mux_av_timeline *mux_av;      /**< ring 输出时间线映射 */
    void *aac;                        /**< zcs_aac_config *（ADTS 配置） */
    uint8_t *mux_buf;                 /**< SPS/PPS 前置与 H.265 AU 构建暂存 */
    size_t mux_buf_cap;
    uint8_t *adts; /**< ADTS 头+载荷暂存（>= ZCS_MPEGTS_ADTS_MAX） */
    size_t adts_cap;
    int *video_armed; /**< 武装标志；首个 sync/关键帧时置位 */
} zcs_mpegts_mux_feed_view;

/** Mux 一条 H.264 Annex-B AU（关键帧前置 SPS/PPS）并经 sink 输出。 */
ZCS_API void zcs_mpegts_feed_h264(const zcs_mpegts_mux_feed_view *f, const uint8_t *annexb,
                                  size_t len, uint32_t dts_ms, int keyframe);
/** Mux 一条 H.265 Annex-B AU（sync 时构建参数集 AU，否则复制 VCL）并经 sink 输出。 */
ZCS_API void zcs_mpegts_feed_h265(const zcs_mpegts_mux_feed_view *f, const uint8_t *annexb,
                                  size_t len, uint32_t dts_ms, int keyframe);
/** 将 @a es 中各 AAC 帧 ADTS 封装并经 sink 输出。 */
ZCS_API void zcs_mpegts_feed_aac(const zcs_mpegts_mux_feed_view *f, int stream_type,
                                 const uint8_t *es, size_t es_len, uint32_t dts_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_MPEGTS_MPEGTS_MUX_FEED_H */
