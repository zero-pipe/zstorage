#ifndef ZCS_EGRESS_PIPELINE_EGRESS_PIPELINE_H
#define ZCS_EGRESS_PIPELINE_EGRESS_PIPELINE_H

/**
 * @file egress_pipeline.h
 * @brief 统一出站泵：读帧并输出协议规范字节。
 *
 * 经 zcs_egress_source 附着读者。直播/VOD pacing 见 egress_pacing.h。
 * 线路径：RTP（RTSP/WebRTC）、FLV tag（RTMP/HTTP-FLV）。
 */
#include "zcs/media/container/container_registry.h"
#include "zcs/egress/pacing/egress_live_policy.h"
#include "zcs/vod/play/flv_egress.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/media/wire_format.h"
#include "zcs/egress/rtp/rtp_muxer.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zcs_vod_buffer_reader;
struct zcs_vod_reader;
struct zcs_media_source;
struct zcs_mux_av_timeline;
struct zcs_egress_clock;
struct zn_poller;

typedef struct zcs_egress_pipeline zcs_egress_pipeline;

/** 原始线路输出（如 RTSP interleaved）。 */
typedef void (*zcs_egress_wire_cb)(const uint8_t *data, size_t len, void *user);

/** FLV/RTMP tag 回调（msg_type 8=audio，9=video）。 */
typedef void (*zcs_egress_flv_tag_cb)(uint8_t msg_type, uint32_t tag_dts_ms, const uint8_t *body,
                                      size_t len, void *user);

typedef struct zcs_egress_flv_tag {
    uint8_t msg_type;
    uint32_t tag_dts_ms;
    const uint8_t *body;
    size_t len;
} zcs_egress_flv_tag;

/** RTP mux 绑定：自有 mux（首选）或借用会话 mux。 */
typedef struct zcs_egress_rtsp_bind {
    zcs_rtp_muxer *mux;
    const zcs_rtp_muxer_opts *opts;
    zcs_rtp_mux_on_rtp on_rtp;
    void *user;
} zcs_egress_rtsp_bind;

/** FLV/RTMP 出站：将 H.264/AAC（及同类）ES 重封装为 tag。 */
typedef struct zcs_egress_flv_bind {
    struct zcs_media_source *source;
    struct zcs_mux_av_timeline *timeline;
    struct zcs_egress_clock *play_clk;
    zcs_egress_flv_tag_cb on_tag;
    void *user;
    int *video_armed;
} zcs_egress_flv_bind;

typedef struct zcs_egress_pipeline_opts {
    zcs_wire_format_id wire;
    zcs_container_id container;
    zcs_egress_source *reader;
    zcs_egress_wire_cb on_wire;
    void *user;
    const zcs_egress_rtsp_bind *rtsp;
    const zcs_egress_flv_bind *flv;
} zcs_egress_pipeline_opts;

/**
 * 按 opts 创建出站泵。
 *
 * 依赖校验：wire=RTP 需 rtsp 绑定（自带 mux 或 opts+on_rtp）；RTMP 需 flv 且
 * on_tag；HTTP_FLV 需 flv。rtsp.mux 传入时借用（不接管），否则内部自建并管理。
 * @return 新泵；参数不满足或内存不足返回 NULL。
 */
ZCS_API zcs_egress_pipeline *zcs_egress_pipeline_create(const zcs_egress_pipeline_opts *opts);
/** 销毁泵：关闭内部 FLV outbox、释放自建的 RTP mux（借用的不释放）与暂存缓冲。 */
ZCS_API void zcs_egress_pipeline_destroy(zcs_egress_pipeline *p);

/** 绑定 I/O poller，使 tag/ES 暂存走 per-poller 池。 */
ZCS_API void zcs_egress_pipeline_bind_poller(zcs_egress_pipeline *p, struct zn_poller *pol);

/** 将直播 gop_queue pump 为 RTP。@return 本轮发出的帧/tag 数。 */
ZCS_API int zcs_egress_pipeline_pump_live(zcs_egress_pipeline *p, int frame_budget,
                                          int read_timeout_ms, unsigned session_no,
                                          const zcs_egress_live_state *live);

/**
 * 将 VOD fifo pump 为 RTP（仅 wire=RTP 有效，否则返回 0）。
 * @param frame_budget <=0 时取默认预算；catchup 状态下自动放大预算
 * @return 本轮发出的帧数
 */
ZCS_API int zcs_egress_pipeline_pump_vod(zcs_egress_pipeline *p,
                                         struct zcs_vod_buffer_reader *vod_rd,
                                         struct zcs_vod_reader *vod_demux, int frame_budget,
                                         unsigned session_no);

/** RTSP PLAY pump：@a vod_rd 非空走 VOD fifo，否则 live gop_queue → RTP。 */
ZCS_API int zcs_egress_pipeline_pump_rtsp(zcs_egress_pipeline *p,
                                          struct zcs_vod_buffer_reader *vod_rd,
                                          struct zcs_vod_reader *vod_demux, int frame_budget,
                                          int read_timeout_ms, unsigned session_no,
                                          const zcs_egress_live_state *live);

/** 将直播 gop_queue pump 为 FLV tag（进 outbox，由 flush 分发给 on_tag）。@return 本轮 tag 数。 */
ZCS_API int zcs_egress_pipeline_pump_flv_live(zcs_egress_pipeline *p, int frame_budget,
                                              int read_timeout_ms, unsigned session_no,
                                              const zcs_egress_live_state *live);

/** 将每会话 FLV outbox flush 到 on_tag（RTMP play tick 末尾）。 */
ZCS_API int zcs_egress_pipeline_flush_flv_outbox(zcs_egress_pipeline *p, int tag_budget);

/** @return 已排队尚未分发的 FLV tag 数。 */
ZCS_API size_t zcs_egress_pipeline_flv_outbox_pending(const zcs_egress_pipeline *p);

/**
 * 拉取一条直播 FLV 媒体 tag（body 在下一次 pull/pump 前有效）。
 * @return 1 有 tag，0 无
 */
ZCS_API int zcs_egress_pipeline_pull_flv_live(zcs_egress_pipeline *p, zcs_egress_flv_tag *out,
                                              int read_timeout_ms, unsigned session_no,
                                              const zcs_egress_live_state *live);

/** 将 VOD fifo pump 为 FLV tag（paced）。@return 本轮 tag 数。 */
ZCS_API int zcs_egress_pipeline_pump_flv_vod(zcs_egress_pipeline *p,
                                             const zcs_flv_vod_egress_bind *vcfg, int frame_budget,
                                             unsigned session_no);

/** @return 1 表示 VOD FLV tag 就绪，0 表示 pacing 未到期或空。 */
ZCS_API int zcs_egress_pipeline_pull_flv_vod(zcs_egress_pipeline *p,
                                             const zcs_flv_vod_egress_bind *vcfg,
                                             zcs_egress_flv_tag *out, unsigned session_no);

/** 滞后 resync 后：将读者 snap 到直播 GOP sync。 */
ZCS_API void zcs_egress_pipeline_snap_live(zcs_egress_pipeline *p);

/** 丢弃 RTP mux 积压、直接跳到最新（追不上直播时的降延迟手段）。 */
ZCS_API void zcs_egress_pipeline_jump_live(zcs_egress_pipeline *p);
/** @return 直播 gop 读者的积压帧数；非直播或未附读者返回 0。 */
ZCS_API size_t zcs_egress_pipeline_gop_lag(const zcs_egress_pipeline *p);
/** @return 内部/借用的 RTP muxer；非 RTP 泵返回 NULL。 */
ZCS_API zcs_rtp_muxer *zcs_egress_pipeline_rtsp_mux(const zcs_egress_pipeline *p);

/**
 * 便捷 pump：按 wire 选择 live FLV 或 live RTP 路径，使用默认预算与 session 0。
 * 仅适用于简单直播场景；需 VOD 或自定义预算请直接调对应 pump_* 。
 */
ZCS_API zn_err_t zcs_egress_pipeline_pump(zcs_egress_pipeline *p, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_PIPELINE_EGRESS_PIPELINE_H */
