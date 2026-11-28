#ifndef ZCS_EGRESS_RTP_RTP_PLAY_PUMP_H
#define ZCS_EGRESS_RTP_RTP_PLAY_PUMP_H

/**
 * @file rtp_play_pump.h
 * @brief 共享 RTP 播放泵与发送队列（RTSP PLAY / WebRTC WHEP 数据面）。
 *
 * 与 @ref zcs_rtp_muxer（slot RTP）配合。RTSP 会话负责信令并实现 TCP interleaved 写回调。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/stream/hub.h"
#include "zcs/egress/pacing/egress_pacing.h"
#include "zcs/egress/rtp/rtp_muxer.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zcs_egress_pipeline;

struct zcs_gop_reader;
struct zcs_vod_buffer_reader;
struct zcs_vod_play_lane;
struct zcs_vod_reader;
struct zn_poller;

#define ZCS_RTP_PLAY_RTPQ_CAP 512u
#define ZCS_RTP_PLAY_RTPQ_HIGH_WATER 128u
#define ZCS_RTP_PLAY_FRAME_BUDGET 32
#define ZCS_RTP_PLAY_FRAME_BUDGET_LIVE 8u
#define ZCS_RTP_PLAY_FRAME_BUDGET_SEEK ZCS_EGRESS_FRAME_BUDGET_CATCHUP
#define ZCS_RTP_PLAY_CATCHUP_LAG ZCS_EGRESS_CATCHUP_LAG
#define ZCS_RTP_PLAY_RESYNC_LAG ZCS_EGRESS_RESYNC_LAG
#define ZCS_RTP_PLAY_RESYNC_LAG_MAX ZCS_EGRESS_RESYNC_LAG_MAX
#define ZCS_RTP_PLAY_RESYNC_COOLDOWN_MS ZCS_EGRESS_RESYNC_COOLDOWN_MS
#define ZCS_RTP_PLAY_RTP_FLUSH 256
#define ZCS_RTP_VOD_CATCHUP_FRAMES_START ZCS_EGRESS_VOD_CATCHUP_START
#define ZCS_RTP_VOD_CATCHUP_FRAMES_SEEK ZCS_EGRESS_VOD_CATCHUP_SEEK
#define ZCS_RTP_VOD_PREFILL_LAG ZCS_EGRESS_VOD_PREFILL_LAG
/** 双轨 RTP-Info 最坏情况缓冲（peer + app + stream ×2）。 */
#define ZCS_RTSP_RTP_INFO_MAX 768u
/** PLAY 200 额外头（Session + Range/Scale + RTP-Info）。 */
#define ZCS_RTSP_PLAY_EXTRA_MAX 1024u

typedef struct zcs_rtp_play_sender zcs_rtp_play_sender;

/** PLAY Range：npt=12.3- / npt=1:00:00.000- / npt=now- */
ZCS_API int zcs_rtsp_parse_range_npt_ms(const char *range_hdr, uint64_t *out_ms, int *out_now);
/**
 * 解析 PLAY 请求的 Scale 头。
 * @param hdr     原始头值；空或非法时返回 current（current 非正时退化为 1.0）
 * @param current 会话当前倍速
 * @return 合法倍速（限制在 (0,16]）；越界/解析失败保留 current
 */
ZCS_API double zcs_rtsp_parse_play_scale(const char *hdr, double current);
/**
 * 计算 VOD PLAY 的 RTP 时间锚点。
 * @param rd      点播读取器；NULL 或无可读帧时直接用 seek_ms
 * @param seek_ms 请求的起始毫秒
 * @return 首个可发帧的 dts_ms（对齐媒体时间轴），否则回退 seek_ms
 */
ZCS_API uint32_t zcs_rtp_play_vod_anchor_ms(struct zcs_vod_buffer_reader *rd, uint32_t seek_ms);

typedef struct zcs_rtsp_play_rtp_info_args {
    const char *host;
    const char *app;
    const char *stream;
    const zcs_media_source *source;
    zcs_rtp_muxer *mux;
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    int audio_rate;
    uint32_t anchor_ms;
    /** 1 = VOD 线性 rtptime（anchor_ms * hz / 1000）；0 = 直播时钟映射。 */
    int vod_linear_rtp;
} zcs_rtsp_play_rtp_info_args;

/**
 * 格式化 PLAY 200 响应里的 RTP-Info 头（按源实际存在的轨道输出 trackID/seq/rtptime）。
 * @param args     轨道时钟、序列号来源与锚点
 * @param rtp_info 输出缓冲（写 NUL；无轨道时为空串）
 * @param cap      缓冲容量，建议 ZCS_RTSP_RTP_INFO_MAX
 */
ZCS_API void zcs_rtsp_play_format_rtp_info(const zcs_rtsp_play_rtp_info_args *args, char *rtp_info,
                                           size_t cap);

typedef void (*zcs_rtp_play_mux_rtp_cb)(zcs_rtp_mux_track track, const uint8_t *rtp, size_t len,
                                        void *user);

typedef struct zcs_rtp_play_mux_handle {
    struct zcs_egress_pipeline *egress;
    zcs_rtp_muxer *mux;
} zcs_rtp_play_mux_handle;

typedef struct zcs_rtp_play_mux_opts {
    zcs_rtp_muxer_opts mux_opts;
    zcs_egress_source *reader;
    zcs_rtp_play_mux_rtp_cb on_rtp;
    void *user;
} zcs_rtp_play_mux_opts;

/**
 * 创建 RTP 播放复用器：内部建 egress pipeline 并取出 rtsp mux，供 RTSP/WHEP 数据面使用。
 * @param out  输出句柄（持 pipeline 与 mux，二者同生命周期）
 * @param opts reader、mux 选项与 on_rtp 回调（必填）
 * @return ZN_OK；参数缺失 ZN_ERR_INVALID；pipeline 创建失败 ZN_ERR_STATE
 */
ZCS_API zn_err_t zcs_rtp_play_mux_create(zcs_rtp_play_mux_handle *out,
                                          const zcs_rtp_play_mux_opts *opts);
/** 销毁 mux 句柄：连带销毁其内部 egress pipeline；对 NULL 安全。 */
ZCS_API void zcs_rtp_play_mux_destroy(zcs_rtp_play_mux_handle *h);

typedef struct zcs_rtp_play_run_ctx {
    zcs_rtp_muxer *mux;
    struct zcs_egress_pipeline *egress;
    zcs_rtp_play_sender *sender;
    struct zcs_gop_reader *gop_reader;
    struct zcs_vod_buffer_reader *vod_reader;
    struct zcs_vod_play_lane *vod_lane;
    const zcs_media_source *source;
    int *close_pending;
    int *destroy_scheduled;
    int *play_config_pending;
    int *play_live_catchup;
    uint64_t *play_lag_resync_ms;
    unsigned session_no;
    int low_latency;
} zcs_rtp_play_run_ctx;

/**
 * 计算本轮 pump 的帧预算：VOD seek/catchup 阶段放大透支，直播按 epoch/追赶状态动态取值。
 * @return 建议的帧数上限（ctx 为 NULL 时返回直播默认值）
 */
ZCS_API int zcs_rtp_play_frame_budget(const zcs_rtp_play_run_ctx *ctx);
/**
 * 计算一次性“追赶”轮次的帧预算（例如 kick 或首次填充时）。
 * @return VOD catchup 用 seek 级预算；直播较保守；其余用默认预算
 */
ZCS_API int zcs_rtp_play_kick_budget(const zcs_rtp_play_run_ctx *ctx);
/**
 * 组装并执行一轮 RTP 播放 pump（内部填 VOD lane、构造内部 pump 结构后调用 pump_run）。
 * @param budget 帧预算；0/负值走 pump_run 的默认策略
 * @param flush  RTP 发送队列刷出预算
 * @return 本轮实际处理的帧数；ctx 为空时返回 0
 */
ZCS_API int zcs_rtp_play_run_pump(zcs_rtp_play_run_ctx *ctx, int budget, int flush);

typedef struct zcs_rtp_play_vod_seek_state {
    zcs_rtp_muxer *mux;
    zcs_rtp_play_sender *sender;
    struct zcs_vod_buffer_reader *vod_reader;
    uint64_t seek_ms;
    double scale;
    int is_replay;
    int was_paused;
    int did_seek;
} zcs_rtp_play_vod_seek_state;

/**
 * 将 VOD seek/pause-恢复的状态落到实处：清发送队列、重算锚点、重设倍速与 catchup。
 * 幂等，只依据 st 内的标志位决定动作；mux 为空时直接返回。
 */
ZCS_API void zcs_rtp_play_apply_vod_seek(zcs_rtp_play_vod_seek_state *st);

/**
 * 生成 VOD PLAY 200 额外头（Session/Range/Scale/RTP-Info）。
 * @param extra  输出缓冲（写 NUL）
 * @param scale  倍速；等于 1.0 时省略 Scale 行
 * @param vod_dur_sec 总时长秒；>0 时 Range 带结束点，否则只写起始
 */
ZCS_API void zcs_rtsp_play_format_vod_play_200(char *extra, size_t extra_cap,
                                               const char *session_id, double scale,
                                               uint64_t seek_ms, double vod_dur_sec,
                                               const char *rtp_info);
/** 生成直播 PLAY 200 额外头（Session + 固定 npt=0.000- + RTP-Info）。 */
ZCS_API void zcs_rtsp_play_format_live_play_200(char *extra, size_t extra_cap,
                                                const char *session_id, const char *rtp_info);

/** 写 interleaved 帧：$ + channel + length + payload。 */
typedef void (*zcs_rtp_play_write_fn)(void *user, uint8_t channel, const uint8_t *payload,
                                      size_t len);

/**
 * 创建 RTP 发送队列（环形槽位，每个槽持一个 zn_buf）。
 * @param write 实际写出回调（interleaved/DTLS 等）；仅当 queued 提交时整条队列复用
 * @param user  回调上下文
 * @param cap   队列容量；为 0 时取 ZCS_RTP_PLAY_RTPQ_CAP
 * @return 新队列；内存不足返回 NULL
 */
ZCS_API zcs_rtp_play_sender *zcs_rtp_play_sender_create(zcs_rtp_play_write_fn write, void *user,
                                                        unsigned cap);
/** 销毁发送队列并 unref 所有未发出的 buf；对 NULL 安全。 */
ZCS_API void zcs_rtp_play_sender_destroy(zcs_rtp_play_sender *s);
/** 清空队列（unref 未发 buf、读写指针归零）；保留队列分配。用于 keyframe/session 重同步。 */
ZCS_API void zcs_rtp_play_sender_reset(zcs_rtp_play_sender *s);
/** 设置/更新会话号，仅用于队列溢出等日志标识；0 表示不打印。 */
ZCS_API void zcs_rtp_play_sender_set_session_no(zcs_rtp_play_sender *s, unsigned session_no);
/** 绑定 I/O poller，使 RTP 发送队列使用 zn_buf_alloc_local。 */
ZCS_API void zcs_rtp_play_sender_set_poller(zcs_rtp_play_sender *s, struct zn_poller *poller);
/** 查询当前发送队列中待发出（已入队未 flush）的包数。 */
ZCS_API size_t zcs_rtp_play_sender_pending(const zcs_rtp_play_sender *s);

/**
 * @param queued 1 = PLAY 期间入队（避免编码器回调重入 flush）。
 */
ZCS_API void zcs_rtp_play_sender_submit(zcs_rtp_play_sender *s, int queued, uint8_t channel,
                                        const uint8_t *payload, size_t len);

/**
 * 刷出发送队列至 write 回调。
 * @param budget        最多发出多少个包
 * @param close_pending 非 0 时直接不刷（会话正在关闭，避免重入）
 * 重入保护：flush 期间再次调用会被忽略。
 */
ZCS_API void zcs_rtp_play_sender_flush(zcs_rtp_play_sender *s, int budget, int close_pending);

/** 直播 bootstrap：发 config、seek_live_idr，首个关键帧 insertConfig；在 sync 点锁定 epoch。 */
ZCS_API int zcs_rtp_play_bootstrap_live(zcs_rtp_muxer *mux, const zcs_media_source *src,
                                        struct zcs_gop_reader *reader, unsigned session_no);

/** 直播边缘 bootstrap：仅 stream config + seek_live_idr（WebRTC WHEP 低延迟）。 */
ZCS_API int zcs_rtp_play_bootstrap_live_edge(zcs_rtp_muxer *mux, const zcs_media_source *src,
                                             struct zcs_gop_reader *reader, unsigned session_no);

/** VOD bootstrap：发 config，从头读 fifo；有 lane 时优先 lane fifo。 */
ZCS_API int zcs_rtp_play_bootstrap_vod(zcs_rtp_muxer *mux, const zcs_media_source *src,
                                       struct zcs_vod_buffer_reader *reader, unsigned session_no,
                                       struct zcs_vod_play_lane *lane, uint32_t anchor_ms);

typedef struct zcs_rtp_play_pump {
    zcs_rtp_muxer *mux;
    zcs_rtp_play_sender *sender;
    struct zcs_gop_reader *gop_reader;
    struct zcs_vod_buffer_reader *vod_reader;
    struct zcs_vod_reader *vod_demux;
    const zcs_media_source *source;
    /** 会话 close/destroy 标志；每轮 pump 检查。 */
    const int *close_flag;
    const int *destroy_flag;
    int *config_pending;
    int *live_catchup_done;
    uint64_t *live_resync_at_ms;
    unsigned session_no;
    int low_latency;
    struct zcs_egress_pipeline *egress;
} zcs_rtp_play_pump;

/** 读 ring/fifo、mux、flush 发送队列；返回本轮处理的帧数。 */
ZCS_API int zcs_rtp_play_pump_run(zcs_rtp_play_pump *p, int frame_budget, int rtp_flush_budget);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_RTP_RTP_PLAY_PUMP_H */
