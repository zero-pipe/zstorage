#ifndef ZMS_EGRESS_RTP_RTP_PLAY_PUMP_H
#define ZMS_EGRESS_RTP_RTP_PLAY_PUMP_H

/**
 * @file rtp_play_pump.h
 * @brief 共享 RTP 播放泵与发送队列（RTSP PLAY / WebRTC WHEP 数据面）。
 *
 * 与 @ref zms_rtp_muxer（slot RTP）配合。RTSP 会话负责信令并实现 TCP interleaved 写回调。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/core/stream/hub.h"
#include "zms/egress/pacing/egress_pacing.h"
#include "zms/egress/rtp/rtp_muxer.h"
#include "zms/egress/source/egress_source.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zms_egress_pipeline;

struct zms_gop_reader;
struct zms_vod_buffer_reader;
struct zms_vod_play_lane;
struct zms_vod_reader;
struct zn_poller;

#define ZMS_RTP_PLAY_RTPQ_CAP 512u
#define ZMS_RTP_PLAY_RTPQ_HIGH_WATER 128u
#define ZMS_RTP_PLAY_FRAME_BUDGET 32
#define ZMS_RTP_PLAY_FRAME_BUDGET_LIVE 8u
#define ZMS_RTP_PLAY_FRAME_BUDGET_SEEK ZMS_EGRESS_FRAME_BUDGET_CATCHUP
#define ZMS_RTP_PLAY_CATCHUP_LAG ZMS_EGRESS_CATCHUP_LAG
#define ZMS_RTP_PLAY_RESYNC_LAG ZMS_EGRESS_RESYNC_LAG
#define ZMS_RTP_PLAY_RESYNC_LAG_MAX ZMS_EGRESS_RESYNC_LAG_MAX
#define ZMS_RTP_PLAY_RESYNC_COOLDOWN_MS ZMS_EGRESS_RESYNC_COOLDOWN_MS
#define ZMS_RTP_PLAY_RTP_FLUSH 256
#define ZMS_RTP_VOD_CATCHUP_FRAMES_START ZMS_EGRESS_VOD_CATCHUP_START
#define ZMS_RTP_VOD_CATCHUP_FRAMES_SEEK ZMS_EGRESS_VOD_CATCHUP_SEEK
#define ZMS_RTP_VOD_PREFILL_LAG ZMS_EGRESS_VOD_PREFILL_LAG
/** 双轨 RTP-Info 最坏情况缓冲（peer + app + stream ×2）。 */
#define ZMS_RTSP_RTP_INFO_MAX 768u
/** PLAY 200 额外头（Session + Range/Scale + RTP-Info）。 */
#define ZMS_RTSP_PLAY_EXTRA_MAX 1024u

typedef struct zms_rtp_play_sender zms_rtp_play_sender;

/** PLAY Range：npt=12.3- / npt=1:00:00.000- / npt=now- */
ZMS_API int zms_rtsp_parse_range_npt_ms(const char *range_hdr, uint64_t *out_ms, int *out_now);
/**
 * 解析 PLAY 请求的 Scale 头。
 * @param hdr     原始头值；空或非法时返回 current（current 非正时退化为 1.0）
 * @param current 会话当前倍速
 * @return 合法倍速（限制在 (0,16]）；越界/解析失败保留 current
 */
ZMS_API double zms_rtsp_parse_play_scale(const char *hdr, double current);
/**
 * 计算 VOD PLAY 的 RTP 时间锚点。
 * @param rd      点播读取器；NULL 或无可读帧时直接用 seek_ms
 * @param seek_ms 请求的起始毫秒
 * @return 首个可发帧的 dts_ms（对齐媒体时间轴），否则回退 seek_ms
 */
ZMS_API uint32_t zms_rtp_play_vod_anchor_ms(struct zms_vod_buffer_reader *rd, uint32_t seek_ms);

typedef struct zms_rtsp_play_rtp_info_args {
    const char *host;
    const char *app;
    const char *stream;
    const zms_media_source *source;
    zms_rtp_muxer *mux;
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    int audio_rate;
    uint32_t anchor_ms;
    /** 1 = VOD 线性 rtptime（anchor_ms * hz / 1000）；0 = 直播时钟映射。 */
    int vod_linear_rtp;
} zms_rtsp_play_rtp_info_args;

/**
 * 格式化 PLAY 200 响应里的 RTP-Info 头（按源实际存在的轨道输出 trackID/seq/rtptime）。
 * @param args     轨道时钟、序列号来源与锚点
 * @param rtp_info 输出缓冲（写 NUL；无轨道时为空串）
 * @param cap      缓冲容量，建议 ZMS_RTSP_RTP_INFO_MAX
 */
ZMS_API void zms_rtsp_play_format_rtp_info(const zms_rtsp_play_rtp_info_args *args, char *rtp_info,
                                           size_t cap);

typedef void (*zms_rtp_play_mux_rtp_cb)(zms_rtp_mux_track track, const uint8_t *rtp, size_t len,
                                        void *user);

typedef struct zms_rtp_play_mux_handle {
    struct zms_egress_pipeline *egress;
    zms_rtp_muxer *mux;
} zms_rtp_play_mux_handle;

typedef struct zms_rtp_play_mux_opts {
    zms_rtp_muxer_opts mux_opts;
    zms_egress_source *reader;
    zms_rtp_play_mux_rtp_cb on_rtp;
    void *user;
} zms_rtp_play_mux_opts;

/**
 * 创建 RTP 播放复用器：内部建 egress pipeline 并取出 rtsp mux，供 RTSP/WHEP 数据面使用。
 * @param out  输出句柄（持 pipeline 与 mux，二者同生命周期）
 * @param opts reader、mux 选项与 on_rtp 回调（必填）
 * @return ZN_OK；参数缺失 ZN_ERR_INVALID；pipeline 创建失败 ZN_ERR_STATE
 */
ZMS_API zn_err_t zms_rtp_play_mux_create(zms_rtp_play_mux_handle *out,
                                          const zms_rtp_play_mux_opts *opts);
/** 销毁 mux 句柄：连带销毁其内部 egress pipeline；对 NULL 安全。 */
ZMS_API void zms_rtp_play_mux_destroy(zms_rtp_play_mux_handle *h);

typedef struct zms_rtp_play_run_ctx {
    zms_rtp_muxer *mux;
    struct zms_egress_pipeline *egress;
    zms_rtp_play_sender *sender;
    struct zms_gop_reader *gop_reader;
    struct zms_vod_buffer_reader *vod_reader;
    struct zms_vod_play_lane *vod_lane;
    const zms_media_source *source;
    int *close_pending;
    int *destroy_scheduled;
    int *play_config_pending;
    int *play_live_catchup;
    uint64_t *play_lag_resync_ms;
    unsigned session_no;
    int low_latency;
} zms_rtp_play_run_ctx;

/**
 * 计算本轮 pump 的帧预算：VOD seek/catchup 阶段放大透支，直播按 epoch/追赶状态动态取值。
 * @return 建议的帧数上限（ctx 为 NULL 时返回直播默认值）
 */
ZMS_API int zms_rtp_play_frame_budget(const zms_rtp_play_run_ctx *ctx);
/**
 * 计算一次性“追赶”轮次的帧预算（例如 kick 或首次填充时）。
 * @return VOD catchup 用 seek 级预算；直播较保守；其余用默认预算
 */
ZMS_API int zms_rtp_play_kick_budget(const zms_rtp_play_run_ctx *ctx);
/**
 * 组装并执行一轮 RTP 播放 pump（内部填 VOD lane、构造内部 pump 结构后调用 pump_run）。
 * @param budget 帧预算；0/负值走 pump_run 的默认策略
 * @param flush  RTP 发送队列刷出预算
 * @return 本轮实际处理的帧数；ctx 为空时返回 0
 */
ZMS_API int zms_rtp_play_run_pump(zms_rtp_play_run_ctx *ctx, int budget, int flush);

typedef struct zms_rtp_play_vod_seek_state {
    zms_rtp_muxer *mux;
    zms_rtp_play_sender *sender;
    struct zms_vod_buffer_reader *vod_reader;
    uint64_t seek_ms;
    double scale;
    int is_replay;
    int was_paused;
    int did_seek;
} zms_rtp_play_vod_seek_state;

/**
 * 将 VOD seek/pause-恢复的状态落到实处：清发送队列、重算锚点、重设倍速与 catchup。
 * 幂等，只依据 st 内的标志位决定动作；mux 为空时直接返回。
 */
ZMS_API void zms_rtp_play_apply_vod_seek(zms_rtp_play_vod_seek_state *st);

/**
 * 生成 VOD PLAY 200 额外头（Session/Range/Scale/RTP-Info）。
 * @param extra  输出缓冲（写 NUL）
 * @param scale  倍速；等于 1.0 时省略 Scale 行
 * @param vod_dur_sec 总时长秒；>0 时 Range 带结束点，否则只写起始
 */
ZMS_API void zms_rtsp_play_format_vod_play_200(char *extra, size_t extra_cap,
                                               const char *session_id, double scale,
                                               uint64_t seek_ms, double vod_dur_sec,
                                               const char *rtp_info);
/** 生成直播 PLAY 200 额外头（Session + 固定 npt=0.000- + RTP-Info）。 */
ZMS_API void zms_rtsp_play_format_live_play_200(char *extra, size_t extra_cap,
                                                const char *session_id, const char *rtp_info);

/** 写 interleaved 帧：$ + channel + length + payload。 */
typedef void (*zms_rtp_play_write_fn)(void *user, uint8_t channel, const uint8_t *payload,
                                      size_t len);

/**
 * 创建 RTP 发送队列（环形槽位，每个槽持一个 zn_buf）。
 * @param write 实际写出回调（interleaved/DTLS 等）；仅当 queued 提交时整条队列复用
 * @param user  回调上下文
 * @param cap   队列容量；为 0 时取 ZMS_RTP_PLAY_RTPQ_CAP
 * @return 新队列；内存不足返回 NULL
 */
ZMS_API zms_rtp_play_sender *zms_rtp_play_sender_create(zms_rtp_play_write_fn write, void *user,
                                                        unsigned cap);
/** 销毁发送队列并 unref 所有未发出的 buf；对 NULL 安全。 */
ZMS_API void zms_rtp_play_sender_destroy(zms_rtp_play_sender *s);
/** 清空队列（unref 未发 buf、读写指针归零）；保留队列分配。用于 keyframe/session 重同步。 */
ZMS_API void zms_rtp_play_sender_reset(zms_rtp_play_sender *s);
/** 设置/更新会话号，仅用于队列溢出等日志标识；0 表示不打印。 */
ZMS_API void zms_rtp_play_sender_set_session_no(zms_rtp_play_sender *s, unsigned session_no);
/** 绑定 I/O poller，使 RTP 发送队列使用 zn_buf_alloc_local。 */
ZMS_API void zms_rtp_play_sender_set_poller(zms_rtp_play_sender *s, struct zn_poller *poller);
/** 查询当前发送队列中待发出（已入队未 flush）的包数。 */
ZMS_API size_t zms_rtp_play_sender_pending(const zms_rtp_play_sender *s);

/**
 * @param queued 1 = PLAY 期间入队（避免编码器回调重入 flush）。
 */
ZMS_API void zms_rtp_play_sender_submit(zms_rtp_play_sender *s, int queued, uint8_t channel,
                                        const uint8_t *payload, size_t len);

/**
 * 刷出发送队列至 write 回调。
 * @param budget        最多发出多少个包
 * @param close_pending 非 0 时直接不刷（会话正在关闭，避免重入）
 * 重入保护：flush 期间再次调用会被忽略。
 */
ZMS_API void zms_rtp_play_sender_flush(zms_rtp_play_sender *s, int budget, int close_pending);

/** 直播 bootstrap：发 config、seek_live_idr，首个关键帧 insertConfig；在 sync 点锁定 epoch。 */
ZMS_API int zms_rtp_play_bootstrap_live(zms_rtp_muxer *mux, const zms_media_source *src,
                                        struct zms_gop_reader *reader, unsigned session_no);

/** 直播边缘 bootstrap：仅 stream config + seek_live_idr（WebRTC WHEP 低延迟）。 */
ZMS_API int zms_rtp_play_bootstrap_live_edge(zms_rtp_muxer *mux, const zms_media_source *src,
                                             struct zms_gop_reader *reader, unsigned session_no);

/** VOD bootstrap：发 config，从头读 fifo；有 lane 时优先 lane fifo。 */
ZMS_API int zms_rtp_play_bootstrap_vod(zms_rtp_muxer *mux, const zms_media_source *src,
                                       struct zms_vod_buffer_reader *reader, unsigned session_no,
                                       struct zms_vod_play_lane *lane, uint32_t anchor_ms);

typedef struct zms_rtp_play_pump {
    zms_rtp_muxer *mux;
    zms_rtp_play_sender *sender;
    struct zms_gop_reader *gop_reader;
    struct zms_vod_buffer_reader *vod_reader;
    struct zms_vod_reader *vod_demux;
    const zms_media_source *source;
    /** 会话 close/destroy 标志；每轮 pump 检查。 */
    const int *close_flag;
    const int *destroy_flag;
    int *config_pending;
    int *live_catchup_done;
    uint64_t *live_resync_at_ms;
    unsigned session_no;
    int low_latency;
    struct zms_egress_pipeline *egress;
} zms_rtp_play_pump;

/** 读 ring/fifo、mux、flush 发送队列；返回本轮处理的帧数。 */
ZMS_API int zms_rtp_play_pump_run(zms_rtp_play_pump *p, int frame_budget, int rtp_flush_budget);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_RTP_RTP_PLAY_PUMP_H */
