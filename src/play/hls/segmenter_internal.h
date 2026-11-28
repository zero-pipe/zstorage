#ifndef ZCS_SRC_PLAY_HLS_SEGMENTER_INTERNAL_H
#define ZCS_SRC_PLAY_HLS_SEGMENTER_INTERNAL_H

/**
 * @file segmenter_internal.h
 * @brief HLS 录制器跨文件共享的私有状态与接口。
 *        供核心（http_hls_segmenter.c）及其 mux 写端（http_hls_fmp4_writer.c、TS 路径）使用；
 *        不属于公开 API。
 */
#include "zcs/play/hls/segmenter.h"
#include "zcs/play/hls/sidecar.h"
#include "zcs/media/codec/param_sets.h"
#include "zcs/media/container/container_registry.h"
#include "zcs/core/gop/queue.h"
#include "zcs/media/clock.h"
#include "zcs/core/stream/hub.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/codec/aac/aac_config.h"
#include "zcs/media/container/hls/hls_fmp4.h"
#include "zm/packager/packager.h"
#include "zb/os/sync.h"
#include "zn/util/timer.h"
#include "zn/platform.h"
#include <stddef.h>
#include <stdint.h>

struct zcs_http_hls_segmenter {
    zcs_media_source *src;
    zcs_http_hls_playlist *maker;
    const zcs_container_muxer_ops *ts_mux_ops;
    void *ts_mux;
    zcs_hls_fmp4 *fmp4_mux;
    zm_packager *zm_pkg; /**< HLS(TS) packager；非 NULL 时优先于 legacy ts_mux */
    int use_fmp4;
    int force_fmp4;
    int save_enabled;
    char save_dir[512];
    unsigned seg_count; /**< HLS 窗口分片数（升级 fMP4 时重建 maker 用） */
    int fmp4_video_track;
    int fmp4_audio_track;
    int fmp4_init_done;
    uint32_t fmp4_seg_origin_dts_ms;
    zcs_gop_reader *reader;
    zb_mutex *mu;
    int closing;
    int enable_audio;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    float seg_duration_sec;
    uint64_t last_hls_req_ms;
    /** TS 当前打开分片的起点 mux dts；用于无关键帧时强制切段。 */
    uint64_t ts_seg_origin_dts_ms;
    int ts_seg_open;
    uint8_t *annexb;
    size_t annexb_cap;
    uint8_t *mux_buf;
    size_t mux_buf_cap;
    uint8_t *adts;
    size_t adts_cap;
    uint8_t hevc_esinfo[64];
    size_t hevc_esinfo_len;
    int aac_sr;
    int aac_ch;
    zcs_aac_config aac;
    int have_aac;
    uint64_t last_mux_dts_ms;
    uint32_t last_video_dts_ms;
    zcs_codec_id video_codec;
    zcs_codec_param_sets params;
    zn_timer *timer;
    zn_poller *timer_poller;
    int http_serve_depth;
    zcs_mux_av_timeline mux_av;
};

/* --- 与 mux writer 共享的核心辅助（定义于 http_hls_segmenter.c）--- */

/** 将 ring 解码时间戳映射到各 track 出站 (mux) 时间线。 */
uint32_t hls_mux_dts_ms(zcs_http_hls_segmenter *rec, zcs_track_type track, uint32_t ring_dts_ms);

/** 将成品媒体分片缓冲推入 HLS maker。 */
int hls_media_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms,
                         int64_t dts_ms, int64_t duration_ms);

/* --- fMP4 writer 入口（定义于 hls_fmp4_writer.c）--- */

int hls_fmp4_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms, int64_t dts_ms,
                        int64_t duration_ms);
void hls_fmp4_serve_flush(zcs_http_hls_segmenter *rec);
void hls_fmp4_ensure_aac_track(zcs_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen);
void hls_fmp4_ensure_opus_track(zcs_http_hls_segmenter *rec);
void hls_fmp4_feed_vpx(zcs_http_hls_segmenter *rec, zcs_codec_id vc, const uint8_t *es, size_t len,
                       uint32_t dts_ms, int keyframe);
void hls_fmp4_feed_aac(zcs_http_hls_segmenter *rec, const uint8_t *es, size_t es_len,
                       uint32_t dts_ms);
void hls_fmp4_feed_av1(zcs_http_hls_segmenter *rec, const uint8_t *es, size_t len, uint32_t dts_ms,
                       int keyframe);
void hls_fmp4_feed_opus(zcs_http_hls_segmenter *rec, const uint8_t *es, size_t len,
                        uint32_t dts_ms);
void hls_fmp4_feed_h26x(zcs_http_hls_segmenter *rec, zcs_codec_id codec, const uint8_t *annexb,
                        size_t len, uint32_t dts_ms, int keyframe);
/** 直播 HLS 落盘（[protocol] hls_save_path）；未开启时为空操作。 */
void hls_save_write_file(zcs_http_hls_segmenter *rec, const char *name, const void *data,
                         size_t len);

/* --- MPEG-TS writer 入口（定义于 hls_ts_writer.c）--- */

/** @return 视频 codec 的 MPEG-TS stream_type（H.264 回退）。 */
int hls_video_psi(zcs_codec_id codec);
void hls_ensure_opus_extradata(zcs_http_hls_segmenter *rec);
void feed_raw_video_es(zcs_http_hls_segmenter *rec, int psi, const uint8_t *es, size_t len,
                       uint32_t dts_ms, int keyframe);
void feed_video_tag_cfg(zcs_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen);
void feed_h264_video_es(zcs_http_hls_segmenter *rec, const uint8_t *annexb, size_t len,
                        uint32_t dts_ms, int keyframe);
void feed_h265_video_es(zcs_http_hls_segmenter *rec, const uint8_t *annexb, size_t len,
                        uint32_t dts_ms, int keyframe);
void feed_audio_tag_cfg(zcs_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen);
void feed_audio_es(zcs_http_hls_segmenter *rec, const uint8_t *es, size_t es_len, uint32_t dts_ms,
                   zcs_codec_id codec);

#endif /* ZCS_SRC_PLAY_HLS_SEGMENTER_INTERNAL_H */
