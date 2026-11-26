#ifndef ZMS_MEDIA_CLOCK_H
#define ZMS_MEDIA_CLOCK_H

/**
 * @file clock.h
 * @brief 入站时间线归一化与出站时钟换算。
 *
 * 直播推流时间戳在 channel 经 @ref zms_track_stamp 与 @ref zms_media_timeline 修订一次。
 * 出站将 ring 毫秒映射为协议时钟（RTP、FLV/RTMP、MPEG-TS PCR、RTCP NTP），不再重跑 DeltaStamp。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/media/codec/codec_id.h"
#include "zms/zms_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_time_base {
    uint32_t num;
    uint32_t den;
} zms_time_base;

/** 毫秒时间基（{1,1000}），供 @ref zms_ts_rescale_q 使用。 */
ZMS_API zms_time_base zms_time_base_ms(void);
/** 由时钟频率构造时间基；clock_hz 为 0 时回退到 MPEG-TS 的 90 kHz。 */
ZMS_API zms_time_base zms_time_base_from_hz(uint32_t clock_hz);

/** 将 @a ts 从 @a from_tb 重标度到 @a to_tb（整数运算，av_rescale_q 风格）。 */
ZMS_API uint64_t zms_ts_rescale_q(uint64_t ts, zms_time_base from_tb, zms_time_base to_tb);

/** 按 rtpmap 时钟（Hz）将 RTP 时间戳换算为毫秒。 */
ZMS_API uint32_t zms_rtp_clock_to_ms(uint32_t rtp_ts, uint32_t clock_hz);
/**
 * 将毫秒换算为 RTP 时钟刻度，与 @ref zms_rtp_clock_to_ms 互逆。
 * @param clock_hz rtpmap 时钟；为 0 时回退 90 kHz。
 * 结果为整数截断，非精确往返。
 */
ZMS_API uint32_t zms_ms_to_rtp_clock(uint32_t ms, uint32_t clock_hz);

/** MPEG-TS / PCR 刻度，90 kHz（ITU-T H.222.0）。 */
ZMS_API uint64_t zms_ms_to_mpegts_90k(uint32_t ms);
/**
 * 90 kHz 刻度转毫秒。
 * @param ticks_90k 可能为负（B 帧 DTS 早于基准时）；负值一律按 0 处理。
 */
ZMS_API uint32_t zms_mpegts_90k_to_ms(int64_t ticks_90k);

/** 由编解码与音频采样率得到标称帧时长。 */
ZMS_API uint32_t zms_codec_frame_duration_ms(zms_codec_id codec, uint32_t sample_rate_hz);

/**
 * 每轨 DeltaStamp，可选跨轨 sync master。
 * 仅用于入站。
 */
typedef struct zms_track_stamp {
    uint32_t max_delta_ms;
    int64_t last_stamp;
    int64_t last_delta;
    int64_t relative_ms;
    int64_t last_dts_in;
    int need_sync;
    struct zms_track_stamp *sync_master;
} zms_track_stamp;

/**
 * 重置单轨 DeltaStamp：清零累计状态，但保留 max_delta_ms（为 0 时取默认 300 ms）
 * 与 sync_master 绑定；last_delta 置 1，避免首帧产生 0 增量。
 */
ZMS_API void zms_track_stamp_reset(zms_track_stamp *s);
/**
 * 把 @a s 绑定到跨轨同步主轨 @a master，并要求随后 count 次修订做 A/V 对齐。
 * @param count 期望对齐次数；<=0 时按 1 处理。仅建立单向引用，不转移所有权。
 */
ZMS_API void zms_track_stamp_sync_to(zms_track_stamp *s, zms_track_stamp *master, int count);

/** @return 增量钳制与 A/V 同步后的相对毫秒。 */
ZMS_API uint32_t zms_track_stamp_revise(zms_track_stamp *s, uint64_t raw_ms);

/** 轻量流时钟：由原始 PTS 间隔推断帧时长。 */
typedef struct zms_stream_clock {
    zms_track_type track;
    uint32_t frame_dur_ms;
    int started;
    uint64_t last_raw_ms;
    uint32_t last_pts_ms;
    uint32_t learned_delta_ms;
} zms_stream_clock;

/** 清空流时钟推断状态，下次 advance 从零重新起步。 */
ZMS_API void zms_stream_clock_reset(zms_stream_clock *c);
/**
 * 依轨道、编解码与采样率设置标称帧时长。
 * 录像视频帧长最短按 40 ms 处理；视频还会给出默认 learn 初值，供后续按实测间隔修正。
 * @param sample_rate_hz 仅音频编解码需要；视频可传 0。
 */
ZMS_API void zms_stream_clock_init(zms_stream_clock *c, zms_track_type track, zms_codec_id codec,
                                   uint32_t sample_rate_hz);

/** @param raw_ms 入站 PTS（毫秒），来自 RTP 重标度或 RTMP tag。 */
ZMS_API uint32_t zms_stream_clock_advance(zms_stream_clock *c, uint64_t raw_ms);

/** 直播入站时间线：视频为主轨，音频经链接 stamp 跟随。 */
typedef struct zms_media_timeline {
    zms_track_stamp vst;
    zms_track_stamp ast;
    int stamp_linked;
    int av_clamp_disabled;
    /** MPEG-TS：demux PTS/DTS 已在共用轴上为毫秒；跳过 DeltaStamp 零原点。 */
    int linear_ms;
    zms_stream_clock video;
    zms_stream_clock audio;
    uint32_t video_clock_hz;
    uint32_t audio_clock_hz;
    zms_codec_id audio_codec;
} zms_media_timeline;

/** 复位入站时间线到默认（视频 90 kHz、音频 44100/AAC）；推流重连时调用。 */
ZMS_API void zms_media_timeline_reset(zms_media_timeline *tl);
/**
 * 设置视频编解码与 rtpmap 时钟并重置视频流时钟。
 * @param clock_hz 为 0 时回退 90 kHz。
 */
ZMS_API void zms_media_timeline_set_video(zms_media_timeline *tl, zms_codec_id codec,
                                          uint32_t clock_hz);
/**
 * 设置音频编解码与采样率/时钟，并顺带把音频 stamp 链接到视频主轨。
 * @param clock_hz 为 0 时回退 44100 Hz。
 */
ZMS_API void zms_media_timeline_set_audio(zms_media_timeline *tl, zms_codec_id codec,
                                          uint32_t clock_hz);

/**
 * 修订一帧视频时间戳并返回相对毫秒。
 * 内部隐式执行 stamp 链接、DeltaStamp 增量钳制与 A/V 对齐钳制，故调用者无需另行处理。
 * @param raw_ms 入站 PTS（毫秒）。
 */
ZMS_API uint32_t zms_media_timeline_video(zms_media_timeline *tl, uint64_t raw_ms);
/** 同 @ref zms_media_timeline_video，处理音频轨；需先经 set_audio 建立音频时钟。 */
ZMS_API uint32_t zms_media_timeline_audio(zms_media_timeline *tl, uint64_t raw_ms);

/** 将音频 stamp 链接到视频主轨做 A/V 同步；幂等，重复调用无副作用。 */
ZMS_API void zms_media_timeline_link_stamps(zms_media_timeline *tl);
/** 关闭 A/V relative_ms 钳制（MPEG-TS PES 原点可能不同；RTP 路径保持默认）。 */
ZMS_API void zms_media_timeline_set_av_clamp(zms_media_timeline *tl, int enabled);

/** 每轨减去自身首 PTS + 单调 DTS（单轨 FLV/HLS mux）。 */
typedef struct zms_mux_timeline {
    uint32_t origin_ms;
    int origin_set;
    uint32_t last_out_ms;
} zms_mux_timeline;

/** 复位单轨 mux 时间线，使下一帧重新成为原点。 */
ZMS_API void zms_mux_timeline_reset(zms_mux_timeline *m);
/**
 * 减去首帧原点并强制单调递增，返回 mux 输出时间戳。
 * 首帧返回 0；ring_pts_ms 早于原点按 0 计，随后按 +1 ms 保证单调（VLC 等要求）。
 */
ZMS_API uint32_t zms_mux_timeline_pts(zms_mux_timeline *m, uint32_t ring_pts_ms);

/**
 * 共享 FLV/MPEG-TS 时间线：各轨减去各自首 PTS，
 * 再强制全局单调 DTS（VLC 等播放器要求）。
 */
typedef struct zms_mux_av_timeline {
    uint32_t origin_ms[2];
    int origin_set[2];
    uint32_t last_out_ms[2];
    uint32_t shared_origin_ms;
    int use_shared_origin;
} zms_mux_av_timeline;

/** 复位双轨共享 mux 时间线（原点、锁定标记、单调状态全部清零）。 */
ZMS_API void zms_mux_av_timeline_reset(zms_mux_av_timeline *m);

/** 将双轨锚定到同一 ring PTS（如首个视频关键帧）。 */
ZMS_API void zms_mux_av_timeline_lock_origin(zms_mux_av_timeline *m, uint32_t origin_ms);

/** 追帧重定原点：移动 origin 但不重置 last_out_ms。 */
ZMS_API void zms_mux_av_timeline_shift_origin(zms_mux_av_timeline *m, uint32_t origin_ms);
/**
 * 双轨共享 mux 时间线换算：按轨道减去原点并仅按该轨单调输出。
 * 共享原点模式下两支使用同一锚点；B 帧 DTS 早于锚点导致的 uint32 回绕按 0 处理，
 * 避免 HLS/DASH 段时长出现巨值。
 * @param track 音频走独立索引（1），其余按视频（0）处理。
 */
ZMS_API uint32_t zms_mux_av_timeline_pts(zms_mux_av_timeline *m, zms_track_type track,
                                         uint32_t ring_pts_ms);

/** Unix 纪元以来的墙钟毫秒（RTCP SR）。 */
ZMS_API uint64_t zms_wall_ms(void);

/** 由墙钟毫秒生成 RFC 3550 NTP 时间戳（RTCP SR）。 */
ZMS_API void zms_wall_ms_to_ntp(uint64_t wall_ms, uint32_t *ntp_sec, uint32_t *ntp_frac);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CLOCK_H */
