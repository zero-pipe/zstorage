/**
 * @file segmenter.c
 * @brief 直播 DASH 分片器（fMP4）。
 *
 * 把源 gop_queue 的码流切成 fMP4 分片并维护 MPD。两条封装路径：
 *   - zm packager（preferred）：H.264/H.265 + AAC 组合时用 zm_dash_packager
 *     直接产出 init/segment/MPD，切片更规范；
 *   - legacy 内建路径：其它编码（VP8/VP9/AV1/Opus 等），自己把 ES 转 mp4 box
 *     喂给 libdash 的 dash_mpd，按 codec 分别处理参数集与时间线。
 * 一旦 zm packager 建起来就走它，否则回落到 legacy。
 *
 * 并发：所有 tick/reader 操作都在 rec->mu 保护下进行；HTTP 请求（serve/copy/
 * ref）与定时器 tick 可能分属不同线程，靠这把锁串行化对 mpd/playlist/reader
 * 的访问。http_serve_depth 记录正在服务中的请求数（供调用方判断是否可安全
 * 销毁）。closing 标志在 destroy 时置位，让在途 tick 提前退出。
 */
#include "zcs/play/dash/segmenter.h"
#include "zcs/play/dash/playlist.h"
#include "zcs/play/hls/sidecar.h"
#include "zcs/media/codec/param_sets.h"
#include "zcs/media/codec/aac/aac_over_rtmp.h"
#include "zcs/media/codec/aac/aac_over_rtp.h"
#include "zcs/media/codec/vpx/vpx_over_rtmp.h"
#include "zcs/media/codec/av1/av1_over_rtmp.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/util/buf_pool.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h264/h264_config.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/codec/h265/h265_config.h"
#include "zcs/media/clock.h"
#include "zcs/core/gop/queue.h"
#include "zcs/egress/pacing/egress_pacing.h"
#include "zcs/egress/recorder/egress_segment_recorder.h"
#include "zcs/core/stream/hub.h"
#include "zcs/media/codec/h264/h264_over_rtmp.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/media/codec/aac/aac_config.h"
#include "dash-mpd.h"
#include "mov-format.h"
#include "webm-vpx.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include "zn/poller/poller.h"
#include "zb/os/sync.h"
#include "zm/packager/packager.h"
#include "bridge/zm_zcs_bridge.h"
#include <stdlib.h>
#include <string.h>

struct zcs_http_dash_segmenter {
    zcs_media_source *src;
    zcs_http_dash_playlist *maker;
    dash_mpd_t *mpd;
    zcs_gop_reader *reader;
    zb_mutex *mu;
    zn_timer *timer;
    zn_poller *timer_poller;
    int http_serve_depth;
    int closing;
    int enable_audio;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    int adapt_video;
    int adapt_audio;
    zcs_avc_config avc;
    zcs_hevc_config hevc;
    zcs_aac_config aac;
    int have_aac;
    uint8_t *mux_buf;
    size_t mux_buf_cap;
    uint8_t *annexb;
    size_t annexb_cap;
    zcs_codec_param_sets params;
    zcs_mux_av_timeline mux_av;
    zcs_codec_id video_codec;
    uint32_t last_video_ring_dts_ms;
    zm_packager *zm_pkg;
    int zm_tried;
    int64_t seg_ms;
    char *zm_mpd;
    size_t zm_mpd_len;
};

/*
 * 默认参数：2 秒切片、保留 3 片、含音频。
 * 与 HLS 保持一致，便于同一份播放器配置在两套协议间切换。
 */
void zcs_http_dash_segmenter_default_opts(zcs_http_dash_segmenter_opts *opts)
{
    if (!opts) {
        return;
    }
    opts->segment_duration_sec = 2.f;
    opts->segment_count = 24;
    opts->enable_audio = 1;
}

static void dash_segmenter_tick_nolock(zcs_http_dash_segmenter *rec);

static void dash_segmenter_tick_cb(void *user)
{
    zcs_http_dash_segmenter *rec = (zcs_http_dash_segmenter *)user;

    if (!rec || !rec->mu) {
        return;
    }
    /* 定时器回调（通常在 timer poller 线程）：持 mu 后再推进，与 HTTP 侧的
     * serve/copy 互斥。closing 期间跳过，避免销毁途中还操作 mpd。 */
    zb_mutex_lock(rec->mu);
    if (!rec->closing) {
        dash_segmenter_tick_nolock(rec);
    }
    zb_mutex_unlock(rec->mu);
}

static int dash_zm_on_init(void *ctx, const char *name, const void *data, size_t len)
{
    (void)ctx;
    (void)name;
    (void)data;
    (void)len;
    return 0;
}

static int dash_zm_on_segment(void *ctx, const char *name, const void *data, size_t len,
                               int64_t pts_ms, int64_t duration_ms, int discontinuity)
{
    zcs_http_dash_segmenter *rec = (zcs_http_dash_segmenter *)ctx;

    (void)discontinuity;
    if (!rec || !rec->maker) {
        return -1;
    }
    /* zm packager 切好一片回调这里：把分片交给 playlist 管理（按 pts/duration
     * 记录到时间线，供后续构建 MPD）。track 参数此处传 0。 */
    return zcs_http_dash_playlist_on_segment(rec->maker, 0, data, len, pts_ms, pts_ms, duration_ms,
                                             name);
}

static int dash_zm_on_playlist(void *ctx, const char *name, const char *text, size_t len)
{
    zcs_http_dash_segmenter *rec = (zcs_http_dash_segmenter *)ctx;
    char *copy;

    (void)name;
    if (!rec || !text || len == 0) {
        return 0;
    }
    /* zm packager 直接产出 MPD 文本；拷一份存到 rec->zm_mpd，serve_mpd 时优先
     * 返回它（比 legacy 自建 MPD 更完整）。realloc 失败则保留旧 MPD。 */
    copy = (char *)realloc(rec->zm_mpd, len + 1);
    if (!copy) {
        return -1;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    rec->zm_mpd = copy;
    rec->zm_mpd_len = len;
    return 0;
}

static uint32_t dash_mux_dts_ms(zcs_http_dash_segmenter *rec, zcs_track_type track,
                                uint32_t ring_dts_ms)
{
    return zcs_mux_av_timeline_pts(&rec->mux_av, track, ring_dts_ms);
}

static int dash_aac_is_adts(const uint8_t *es, size_t es_len)
{
    /* ADTS 同步字：0xFFFx（前 12 位全 1）。有的源 AAC 帧已带 ADTS 头（可含多帧），
     * 需要先拆帧；不带则视为裸帧直接喂。 */
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

static void dash_feed_video_cfg(zcs_http_dash_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    const uint8_t *extra = NULL;
    size_t extra_len = 0;
    int w = 0;
    int h = 0;

    if (!rec || !rec->mpd || !cfg || clen < 2) {
        return;
    }
    /* 从 FLV 视频 config 判定编码，再按编码抽取 mp4 adaptation set 需要的
     * codec-specific extradata（H264→avcC，H265→hvcC，VP8/9/AV1→vpcC/av1C）。
     * 宽度/高度优先用源上报值。只有成功建出 adaptation set 才置 adapt_video>=0，
     * 否则后续帧会在 adapt_video<0 检查处被丢弃。 */
    rec->video_codec = zcs_flv_video_config_codec(cfg, clen);
    if (rec->video_codec == ZCS_CODEC_INVALID && rec->src &&
        rec->src->video.codec != ZCS_CODEC_INVALID) {
        rec->video_codec = rec->src->video.codec;
    }
    if (rec->video_codec == ZCS_CODEC_H264 &&
        zcs_rtmp_avc_extradata(cfg, clen, &extra, &extra_len) && extra && extra_len > 0) {
        if (rec->src) {
            w = (int)rec->src->video.width;
            h = (int)rec->src->video.height;
        }
        (void)zcs_avc_config_load_record(&rec->avc, extra, extra_len);
        (void)zcs_codec_param_sets_cache_flv_cfg(&rec->params, cfg, clen);
        rec->adapt_video =
            dash_mpd_add_video_adaptation_set(rec->mpd, zcs_http_dash_playlist_prefix(rec->maker),
                                              MOV_OBJECT_H264, w, h, extra, extra_len);
    } else if (rec->video_codec == ZCS_CODEC_H265 &&
               zcs_h265_video_config_hvcc(cfg, clen, &extra, &extra_len) && extra &&
               extra_len > 0) {
        if (rec->src) {
            w = (int)rec->src->video.width;
            h = (int)rec->src->video.height;
        }
        (void)zcs_hevc_config_load_record(&rec->hevc, extra, extra_len);
        (void)zcs_codec_param_sets_cache_flv_cfg(&rec->params, cfg, clen);
        rec->adapt_video =
            dash_mpd_add_video_adaptation_set(rec->mpd, zcs_http_dash_playlist_prefix(rec->maker),
                                              MOV_OBJECT_H265, w, h, extra, extra_len);
        if (rec->adapt_video >= 0) {
            zn_info("DASH recorder: H265 adaptation set ready w=%d h=%d hvcc=%u", w, h,
                     (unsigned)extra_len);
        }
    } else if ((rec->video_codec == ZCS_CODEC_VP8 || rec->video_codec == ZCS_CODEC_VP9) &&
               clen >= 8) {
        const uint8_t *vpxc = NULL;
        size_t vpxc_len = 0;
        uint8_t mov_obj = rec->video_codec == ZCS_CODEC_VP9 ? MOV_OBJECT_VP9 : MOV_OBJECT_VP8;

        if (zcs_vpx_over_rtmp_config_extradata(rec->video_codec, cfg, clen, &vpxc, &vpxc_len) &&
            vpxc && vpxc_len >= 8) {
            if (rec->src) {
                w = (int)rec->src->video.width;
                h = (int)rec->src->video.height;
            }
            rec->adapt_video = dash_mpd_add_video_adaptation_set(
                rec->mpd, zcs_http_dash_playlist_prefix(rec->maker), mov_obj, w > 0 ? w : 640,
                h > 0 ? h : 480, vpxc, vpxc_len);
            if (rec->adapt_video >= 0) {
                zn_info("DASH recorder: %s adaptation set ready w=%d h=%d",
                         zcs_codec_name(rec->video_codec), w, h);
            }
        }
    } else if (rec->video_codec == ZCS_CODEC_AV1) {
        const uint8_t *av1c = NULL;
        size_t av1c_len = 0;

        if (zcs_av1_over_rtmp_config_extradata(cfg, clen, &av1c, &av1c_len) && av1c &&
            av1c_len >= 4) {
            if (rec->src) {
                w = (int)rec->src->video.width;
                h = (int)rec->src->video.height;
            }
            rec->adapt_video = dash_mpd_add_video_adaptation_set(
                rec->mpd, zcs_http_dash_playlist_prefix(rec->maker), MOV_OBJECT_AV1,
                w > 0 ? w : 640, h > 0 ? h : 480, av1c, av1c_len);
            if (rec->adapt_video >= 0) {
                zn_info("DASH recorder: AV1 adaptation set ready w=%d h=%d", w, h);
            }
        }
    }
}

static int dash_build_vpx_extra(zcs_codec_id vc, const uint8_t *keyframe, size_t len, uint8_t *out,
                                size_t cap, int *width, int *height)
{
    struct webm_vpx_t vpx;
    int w = 0;
    int h = 0;
    int n;

    if (!keyframe || len < 10 || !out || cap < 8) {
        return -1;
    }
    memset(&vpx, 0, sizeof(vpx));
    if (vc == ZCS_CODEC_VP9) {
        if (webm_vpx_codec_configuration_record_from_vp9(&vpx, &w, &h, keyframe, len) < 0) {
            return -1;
        }
    } else if (vc == ZCS_CODEC_VP8) {
        if (webm_vpx_codec_configuration_record_from_vp8(&vpx, &w, &h, keyframe, len) < 0) {
            return -1;
        }
    } else {
        return -1;
    }
    if (width) {
        *width = w;
    }
    if (height) {
        *height = h;
    }
    n = webm_vpx_codec_configuration_record_save(&vpx, out, cap);
    return n > 0 ? n : -1;
}

static void dash_build_opus_extra(int channels, int sample_rate, uint8_t *out, size_t cap,
                                  size_t *out_len)
{
    uint32_t sr;

    if (!out || cap < 19 || !out_len) {
        return;
    }
    memset(out, 0, 19);
    memcpy(out, "OpusHead", 8);
    out[8] = 1;
    out[9] = (uint8_t)(channels > 0 ? channels : 2);
    out[10] = 0x38;
    out[11] = 0x01;
    sr = sample_rate > 0 ? (uint32_t)sample_rate : 48000u;
    out[12] = (uint8_t)(sr & 0xff);
    out[13] = (uint8_t)((sr >> 8) & 0xff);
    out[14] = (uint8_t)((sr >> 16) & 0xff);
    out[15] = (uint8_t)((sr >> 24) & 0xff);
    *out_len = 19;
}

static void dash_ensure_av1_adaptation(zcs_http_dash_segmenter *rec, const uint8_t *obu, size_t len)
{
    uint8_t av1c[2048];
    int w = 0;
    int h = 0;
    int n;

    if (!rec || !rec->mpd || rec->adapt_video >= 0 || !obu || len == 0) {
        return;
    }
    n = zcs_av1_extradata_from_obu(obu, len, av1c, sizeof(av1c), &w, &h);
    if (n <= 0) {
        return;
    }
    rec->video_codec = ZCS_CODEC_AV1;
    rec->adapt_video = dash_mpd_add_video_adaptation_set(
        rec->mpd, zcs_http_dash_playlist_prefix(rec->maker), MOV_OBJECT_AV1, w > 0 ? w : 640,
        h > 0 ? h : 480, av1c, (size_t)n);
    if (rec->adapt_video >= 0) {
        rec->sent_video_cfg = 1;
        zn_info("DASH recorder: AV1 adaptation set ready w=%d h=%d (keyframe)", w, h);
    }
}

static void dash_ensure_vpx_adaptation(zcs_http_dash_segmenter *rec, zcs_codec_id vc,
                                       const uint8_t *keyframe, size_t len)
{
    uint8_t vpxc[32];
    int w = 0;
    int h = 0;
    int n;
    uint8_t mov_obj;

    if (!rec || !rec->mpd || rec->adapt_video >= 0 || !keyframe || len == 0) {
        return;
    }
    n = dash_build_vpx_extra(vc, keyframe, len, vpxc, sizeof(vpxc), &w, &h);
    if (n <= 0) {
        return;
    }
    mov_obj = vc == ZCS_CODEC_VP9 ? MOV_OBJECT_VP9 : MOV_OBJECT_VP8;
    rec->video_codec = vc;
    rec->adapt_video = dash_mpd_add_video_adaptation_set(
        rec->mpd, zcs_http_dash_playlist_prefix(rec->maker), mov_obj, w > 0 ? w : 640,
        h > 0 ? h : 480, vpxc, (size_t)n);
    if (rec->adapt_video >= 0) {
        rec->sent_video_cfg = 1;
        zn_info("DASH recorder: %s adaptation set ready w=%d h=%d (keyframe)", zcs_codec_name(vc),
                 w, h);
    }
}

static void dash_ensure_opus_adaptation(zcs_http_dash_segmenter *rec)
{
    uint8_t opus_head[32];
    size_t head_len = 0;
    int ch = 2;
    int sr = 48000;

    if (!rec || !rec->mpd || rec->adapt_audio >= 0 || !rec->enable_audio) {
        return;
    }
    if (rec->src && rec->src->audio.ready) {
        ch = rec->src->audio.channels > 0 ? rec->src->audio.channels : 2;
        sr = rec->src->audio.sample_rate > 0 ? rec->src->audio.sample_rate : 48000;
    }
    dash_build_opus_extra(ch, sr, opus_head, sizeof(opus_head), &head_len);
    rec->adapt_audio =
        dash_mpd_add_audio_adaptation_set(rec->mpd, zcs_http_dash_playlist_prefix(rec->maker),
                                          MOV_OBJECT_OPUS, ch, 16, sr, opus_head, head_len);
    if (rec->adapt_audio >= 0) {
        rec->sent_audio_cfg = 1;
        zn_info("DASH recorder: Opus adaptation set ready rate=%d ch=%d", sr, ch);
    }
}

static void dash_feed_audio_cfg(zcs_http_dash_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;
    int sr = 44100;
    int ch = 2;
    zcs_codec_id ac;

    if (!rec || !rec->mpd || !cfg || clen < 2) {
        return;
    }
    ac = zcs_flv_tag_audio_codec(cfg, clen);
    if (ac == ZCS_CODEC_OPUS) {
        dash_ensure_opus_adaptation(rec);
        return;
    }
    if (ac != ZCS_CODEC_AAC || clen < 4) {
        return;
    }
    if (!zcs_rtmp_aac_extradata(cfg, clen, &asc, &asc_len) || !asc || asc_len == 0) {
        return;
    }
    zcs_aac_config_set_defaults(&rec->aac, 44100, 2);
    if (!zcs_aac_config_load_asc(&rec->aac, asc, asc_len)) {
        return;
    }
    rec->have_aac = 1;
    sr = zcs_aac_config_sample_rate(&rec->aac);
    ch = zcs_aac_config_channels(&rec->aac);
    if (ch <= 0) {
        ch = 2;
    }
    rec->adapt_audio =
        dash_mpd_add_audio_adaptation_set(rec->mpd, zcs_http_dash_playlist_prefix(rec->maker),
                                          MOV_OBJECT_AAC, ch, 16, sr, asc, asc_len);
}

static void dash_feed_h264(zcs_http_dash_segmenter *rec, const uint8_t *annexb, size_t len,
                           uint32_t dts_ms, int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts_ms;
    int flags;
    int sync;
    int idr;

    if (!rec || rec->adapt_video < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zcs_h264_annexb_is_sync_key(annexb, len);
    idr = keyframe || zcs_h264_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        /* 首个关键帧才“武装”：之前没有可解码起点，切出来的分片播放器无法起播。
         * 武装时锁定 A/V 时间线原点，使音视频共用同一基准避免错位。 */
        rec->video_armed = 1;
        zcs_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        zn_info("DASH recorder: first H264 IDR ts=%u", (unsigned)dts_ms);
    }
    if (!zcs_buf_pool_slot_resize(&rec->mux_buf, &rec->mux_buf_cap, len + 65536u)) {
        return;
    }
    /* fMP4 参数集在 init 段；media 段勿 prepend SPS/PPS（TS/HLS 习惯，会导致 DASH 解码失败） */
    n = zcs_avc_config_annexb_to_mp4(&rec->avc, annexb, len, rec->mux_buf, rec->mux_buf_cap, &vcl,
                                     &update);
    if (n <= 0) {
        return;
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZCS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    /* 仅在 IDR 切段；sync_key（含非 IDR）会导致约 500ms 碎段，ffplay 追帧卡顿 */
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, rec->mux_buf, (size_t)n, rel_dts_ms,
                         rel_dts_ms, flags);
}

static void dash_feed_h265(zcs_http_dash_segmenter *rec, const uint8_t *annexb, size_t len,
                           uint32_t dts_ms, int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts_ms;
    int flags;
    int sync;
    int idr;
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;

    if (!rec || rec->adapt_video < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zcs_h265_annexb_is_sync_key(annexb, len);
    idr = keyframe || zcs_h265_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        rec->video_armed = 1;
        zcs_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        zn_info("DASH recorder: first H265 sync ts=%u", (unsigned)dts_ms);
    }
    if (!zcs_buf_pool_slot_resize(&rec->mux_buf, &rec->mux_buf_cap, len + 65536u)) {
        return;
    }
    if (sync && rec->params.sps_len > 0 && rec->params.pps_len > 0) {
        size_t full_len = 0;
        /* 关键帧且已有参数集：把 VPS/SPS/PPS 前置拼成本帧完整 AU 再转 mp4 box，
         * 保证每个切片起点自带参数集（DASH 播放器可能从任意分片起播）。 */
        if (zcs_h265_annexb_build_rtp_au(rec->params.vps_len ? rec->params.vps : NULL,
                                         rec->params.vps_len, rec->params.sps, rec->params.sps_len,
                                         rec->params.pps, rec->params.pps_len, annexb, len, 1,
                                         rec->annexb, rec->annexb_cap, &full_len) == ZN_OK &&
            full_len > 0) {
            mux_ptr = rec->annexb;
            mux_len = full_len;
        }
    } else {
        size_t vcl_len = 0;
        /* 非起始帧/无参数集：只取 VCL（图像数据）部分转 mp4，剥掉内联参数集
         * 避免与 init 段重复。 */
        if (zcs_h265_annexb_copy_vcl(annexb, len, rec->annexb, rec->annexb_cap, &vcl_len) ==
                ZN_OK &&
            vcl_len > 0) {
            mux_ptr = rec->annexb;
            mux_len = vcl_len;
        }
    }
    n = zcs_hevc_config_annexb_to_mp4(&rec->hevc, mux_ptr, mux_len, rec->mux_buf, rec->mux_buf_cap,
                                      &vcl, &update);
    if (n <= 0) {
        return;
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZCS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, rec->mux_buf, (size_t)n, rel_dts_ms,
                         rel_dts_ms, flags);
}

typedef struct {
    zcs_http_dash_segmenter *rec;
    uint32_t base_dts_ms;
    unsigned idx;
    uint32_t au_dur_ms;
} dash_aac_feed_ctx;

static uint32_t dash_aac_au_duration_ms(const zcs_http_dash_segmenter *rec)
{
    int sr;

    if (!rec || !rec->have_aac) {
        return 23u;
    }
    sr = zcs_aac_config_sample_rate(&rec->aac);
    if (sr <= 0) {
        sr = 48000;
    }
    return (1024u * 1000u + (uint32_t)sr - 1u) / (uint32_t)sr;
}

static int dash_feed_aac_au(const uint8_t *au, size_t len, void *user)
{
    dash_aac_feed_ctx *ctx = (dash_aac_feed_ctx *)user;
    int64_t rel_dts_ms;
    const uint8_t *raw = au;
    size_t raw_len = len;

    if (!ctx || !ctx->rec || !au || len == 0) {
        return -1;
    }
    if (zcs_aac_es_to_raw(au, len, &raw, &raw_len) != ZN_OK || !raw || raw_len == 0) {
        return -1;
    }
    {
        uint32_t ats = ctx->base_dts_ms + ctx->idx * ctx->au_dur_ms;

        /* 音频 fmp4 勿超前视频太多，否则播放 A-V 严重错位 */
        if (ctx->rec->last_video_ring_dts_ms > 0 &&
            ats > ctx->rec->last_video_ring_dts_ms + 4000u) {
            /* 音频时间戳比视频领先超过 4s：说明视频落后/卡顿，继续灌音频只会
             * 让分片里音频跑在前面导致错位，直接丢弃这段音频等视频追上。 */
            return 0;
        }
        rel_dts_ms = (int64_t)dash_mux_dts_ms(ctx->rec, ZCS_TRACK_AUDIO, ats);
    }
    (void)dash_mpd_input(ctx->rec->mpd, ctx->rec->adapt_audio, raw, raw_len, rel_dts_ms, rel_dts_ms,
                         0);
    ctx->idx++;
    return 0;
}

static void dash_feed_opus(zcs_http_dash_segmenter *rec, const uint8_t *es, size_t es_len,
                           uint32_t dts_ms)
{
    int64_t rel_dts_ms;

    if (!rec || rec->adapt_audio < 0 || !rec->enable_audio || !rec->video_armed || !es ||
        es_len == 0) {
        return;
    }
    /* video_armed 门控：必须视频已武装（时间线原点已锁）才发音频，否则没有共同
     * 时间基准。音频超前视频 4s 以上同样丢弃（见 dash_feed_aac_au）。 */
    if (rec->last_video_ring_dts_ms > 0 && dts_ms > rec->last_video_ring_dts_ms + 4000u) {
        return;
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZCS_TRACK_AUDIO, dts_ms);
    (void)dash_mpd_input(rec->mpd, rec->adapt_audio, es, es_len, rel_dts_ms, rel_dts_ms, 0);
}

static void dash_feed_av1(zcs_http_dash_segmenter *rec, const uint8_t *es, size_t len,
                          uint32_t dts_ms, int keyframe)
{
    int64_t rel_dts_ms;
    int flags;

    if (!rec || !es || len == 0) {
        return;
    }
    if (!keyframe) {
        zcs_gop_slot slot;
        /* 用 gop_slot_refresh_play_key 复核关键帧：AV1 没有命令式关键帧标记，
         * 得看 OBU 里 frame_type/是否 RAP。不是关键帧就不切段。 */
        memset(&slot, 0, sizeof(slot));
        slot.codec = ZCS_CODEC_AV1;
        slot.track = ZCS_TRACK_VIDEO;
        slot.data = (uint8_t *)es;
        slot.len = len;
        slot.keyframe = keyframe;
        zcs_gop_slot_refresh_play_key(&slot);
        keyframe = slot.keyframe;
    }
    /* AV1 的 adaptation set 往往要等首帧才能抽出 av1C（configure 里没有），
     * 所以这里再兜底尝试一次。 */
    dash_ensure_av1_adaptation(rec, es, len);
    if (rec->adapt_video < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zcs_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        zn_info("DASH recorder: first AV1 key ts=%u", (unsigned)dts_ms);
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZCS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    flags = keyframe ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, es, len, rel_dts_ms, rel_dts_ms, flags);
}

static void dash_feed_vpx(zcs_http_dash_segmenter *rec, zcs_codec_id vc, const uint8_t *es,
                          size_t len, uint32_t dts_ms, int keyframe)
{
    int64_t rel_dts_ms;
    int flags;

    if (!rec || !es || len == 0) {
        return;
    }
    if (!keyframe) {
        zcs_gop_slot slot;
        memset(&slot, 0, sizeof(slot));
        slot.codec = vc;
        slot.track = ZCS_TRACK_VIDEO;
        slot.data = (uint8_t *)es;
        slot.len = len;
        slot.keyframe = keyframe;
        zcs_gop_slot_refresh_play_key(&slot);
        keyframe = slot.keyframe;
    }
    dash_ensure_vpx_adaptation(rec, vc, es, len);
    if (rec->adapt_video < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zcs_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        zn_info("DASH recorder: first %s key ts=%u", zcs_codec_name(vc), (unsigned)dts_ms);
    }
    rel_dts_ms = (int64_t)dash_mux_dts_ms(rec, ZCS_TRACK_VIDEO, dts_ms);
    rec->last_video_ring_dts_ms = dts_ms;
    flags = keyframe ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)dash_mpd_input(rec->mpd, rec->adapt_video, es, len, rel_dts_ms, rel_dts_ms, flags);
}

static void dash_feed_aac(zcs_http_dash_segmenter *rec, const uint8_t *es, size_t es_len,
                          uint32_t dts_ms)
{
    dash_aac_feed_ctx ctx;

    if (!rec || rec->adapt_audio < 0 || !rec->enable_audio || !rec->video_armed || !es ||
        es_len == 0) {
        return;
    }
    if (!rec->have_aac) {
        return;
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.rec = rec;
    ctx.base_dts_ms = dts_ms;
    ctx.au_dur_ms = dash_aac_au_duration_ms(rec);
    /* ADTS 帧里可能含多个 AAC frame：逐个拆出、各自算时间戳（按 au_dur 递增）；
     * 不带 ADTS 的裸帧则整段当一个 AU。 */
    if (dash_aac_is_adts(es, es_len)) {
        (void)zcs_aac_es_foreach_frame(es, es_len, dash_feed_aac_au, &ctx);
    } else {
        (void)dash_feed_aac_au(es, es_len, &ctx);
    }
}

zcs_http_dash_segmenter *zcs_http_dash_segmenter_create(zcs_media_source *src,
                                                        const zcs_http_dash_segmenter_opts *opts)
{
    zcs_http_dash_segmenter_opts def;
    zcs_http_dash_segmenter *rec;
    zcs_http_dash_playlist_opts mopts;
    const zcs_http_dash_segmenter_opts *o;

    if (!src || !src->gop_queue) {
        return NULL;
    }
    zcs_http_dash_segmenter_default_opts(&def);
    o = opts ? opts : &def;

    rec = (zcs_http_dash_segmenter *)calloc(1, sizeof(*rec));
    if (!rec) {
        return NULL;
    }

    memset(&mopts, 0, sizeof(mopts));
    mopts.segment_duration_sec = o->segment_duration_sec;
    mopts.segment_count = o->segment_count;
    mopts.prefix = src->stream;
    rec->maker = zcs_http_dash_playlist_create(&mopts);
    rec->mpd = rec->maker ? zcs_http_dash_playlist_mpd(rec->maker) : NULL;
    if (!rec->maker || !rec->mpd) {
        zcs_http_dash_segmenter_destroy(rec);
        return NULL;
    }

    rec->src = src;
    rec->seg_ms = (int64_t)(o->segment_duration_sec * 1000.f);
    if (rec->seg_ms <= 0) {
        rec->seg_ms = 2000;
    }
    rec->enable_audio = o->enable_audio;
    rec->adapt_video = rec->adapt_audio = -1;
    rec->mu = zb_mutex_create(0);
    rec->reader = zcs_gop_reader_attach(src->gop_queue);
    if (!rec->mu || !rec->reader) {
        zcs_http_dash_segmenter_destroy(rec);
        return NULL;
    }
    /* 从最近的直播关键帧起读（不回溯历史，DASH 只发新鲜切片），并把 A/V
     * 时间线原点复位。mux/annexb 缓冲初始 256KB，之后按帧大小自动扩。 */
    zcs_gop_reader_seek_live_key(rec->reader);
    zcs_mux_av_timeline_reset(&rec->mux_av);
    rec->mux_buf_cap = 256 * 1024;
    rec->mux_buf = (uint8_t *)malloc(rec->mux_buf_cap);
    rec->annexb_cap = rec->mux_buf_cap;
    rec->annexb = (uint8_t *)malloc(rec->annexb_cap);
    if (!rec->mux_buf || !rec->annexb) {
        zcs_http_dash_segmenter_destroy(rec);
        return NULL;
    }
    (void)zcs_media_source_segment_rec_set(src, ZCS_SEGMENT_REC_DASH, rec);
    zn_info("DASH recorder start: %s/%s", src->app, src->stream);
    return rec;
}

/*
 * 销毁。与 HLS 一样存在跨线程时序问题：tick 跑在主 poller 线程，destroy 常在
 * 推流线程（publish_fini/reregister）调用；`zn_timer_stop` 无法撤回**已派发**
 * 的回调，若此处立即 free(rec)，在途 tick 会访问已释放内存。因此当定时器属于
 * 其它正在运行的 poller 时，把清理投递到该 poller 串行执行。
 */
static void dash_segmenter_destroy_now(zcs_http_dash_segmenter *rec);

static void dash_segmenter_destroy_deferred(void *user)
{
    dash_segmenter_destroy_now((zcs_http_dash_segmenter *)user);
}

void zcs_http_dash_segmenter_destroy(zcs_http_dash_segmenter *rec)
{
    zn_poller *pol;

    if (!rec) {
        return;
    }
    if (rec->timer) {
        zn_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    pol = rec->timer_poller;
    if (pol && zn_poller_is_running(pol) && !zn_poller_is_current_thread(pol)) {
        /* 先置 closing，避免清理任务到达前 tick/HTTP 继续推进。 */
        if (rec->mu) {
            zb_mutex_lock(rec->mu);
            rec->closing = 1;
            zb_mutex_unlock(rec->mu);
        }
        if (zn_poller_async(pol, dash_segmenter_destroy_deferred, rec, 0) == ZN_OK) {
            return;
        }
    }
    dash_segmenter_destroy_now(rec);
}

static void dash_segmenter_destroy_now(zcs_http_dash_segmenter *rec)
{
    zcs_media_source *src;
    zcs_http_dash_playlist *maker;
    zcs_gop_reader *reader;
    zb_mutex *mu;
    zm_packager *zm_pkg;
    char *zm_mpd;

    if (!rec) {
        return;
    }
    if (rec->timer) {
        zn_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    src = rec->src;
    maker = rec->maker;
    reader = rec->reader;
    mu = rec->mu;
    zm_pkg = rec->zm_pkg;
    zm_mpd = rec->zm_mpd;
    if (mu) {
        zb_mutex_lock(mu);
        rec->closing = 1;
        /* closing 置位后，任何在途 tick/HTTP serve 都会在读 mpd/maker 时提前
         * 退出；把指针清空并 detach reader 放到锁内，确保此刻没有线程还在用。 */
        if (src && zcs_media_source_segment_rec_get(src, ZCS_SEGMENT_REC_DASH) == rec) {
            /* 延迟销毁期间同名录制器可能已重建：只摘除自己，避免抹掉新录制器。 */
            (void)zcs_media_source_segment_rec_set(src, ZCS_SEGMENT_REC_DASH, NULL);
        }
        rec->maker = NULL;
        rec->mpd = NULL;
        rec->reader = NULL;
        rec->zm_pkg = NULL;
        rec->zm_mpd = NULL;
        rec->mu = NULL;
        zb_mutex_unlock(mu);
    } else if (src && zcs_media_source_segment_rec_get(src, ZCS_SEGMENT_REC_DASH) == rec) {
        (void)zcs_media_source_segment_rec_set(src, ZCS_SEGMENT_REC_DASH, NULL);
    }
    if (reader) {
        zcs_gop_reader_detach(reader);
    }
    if (zm_pkg) {
        zm_packager_close(zm_pkg);
    }
    free(zm_mpd);
    if (maker) {
        zcs_http_dash_playlist_destroy(maker);
    }
    free(rec->mux_buf);
    free(rec->annexb);
    zcs_codec_param_sets_clear(&rec->params);
    zb_mutex_destroy(mu);
    free(rec);
}

zcs_http_dash_playlist *zcs_http_dash_segmenter_playlist(zcs_http_dash_segmenter *rec)
{
    return rec ? rec->maker : NULL;
}

static zn_poller *dash_segmenter_timer_poller(zn_poller *fallback)
{
    zn_poller *main_pol = zcs_http_hls_main_poller();
    return main_pol ? main_pol : fallback;
}

void zcs_http_dash_segmenter_bind_timer(zcs_http_dash_segmenter *rec, zn_poller *poller)
{
    zn_poller *bind_pol;

    if (!rec) {
        return;
    }
    bind_pol = dash_segmenter_timer_poller(poller);
    if (!bind_pol) {
        return;
    }
    if (rec->mu) {
        zb_mutex_lock(rec->mu);
    }
    if (rec->closing) {
        rec->timer_poller = bind_pol;
        if (rec->mu) {
            zb_mutex_unlock(rec->mu);
        }
        return;
    }
    /* 幂等：与 HLS 相同，定时器已在目标 poller 时不重启。 */
    if (rec->timer && rec->timer_poller == bind_pol) {
        if (rec->mu) {
            zb_mutex_unlock(rec->mu);
        }
        return;
    }
    rec->timer_poller = bind_pol;
    if (rec->timer) {
        zn_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    rec->timer = zn_timer_start(bind_pol, 10, 1, dash_segmenter_tick_cb, rec);
    if (rec->mu) {
        zb_mutex_unlock(rec->mu);
    }
}

void zcs_http_dash_segmenter_http_enter(zcs_http_dash_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    /* 记录“有 HTTP 请求在用本 record”：调用方（销毁路径）可据此判断能否安全
     * 释放；进入/离开都持锁，深度计数由 tick 之外的 serve/copy/ref 增减。 */
    zb_mutex_lock(rec->mu);
    rec->http_serve_depth++;
    zb_mutex_unlock(rec->mu);
}

void zcs_http_dash_segmenter_http_leave(zcs_http_dash_segmenter *rec, zn_poller *poller)
{
    (void)poller;
    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (rec->http_serve_depth > 0) {
        --rec->http_serve_depth;
    }
    zb_mutex_unlock(rec->mu);
}

void zcs_http_dash_segmenter_touch(zcs_http_dash_segmenter *rec)
{
    if (rec) {
        zcs_http_dash_segmenter_tick(rec);
    }
}

/** 仅在 MPD 请求时轻量推进；分片响应后不 pump，避免覆盖播放器仍请求的缓存 */
static void dash_segmenter_serve_pump_mpd(zcs_http_dash_segmenter *rec)
{
    int i;

    if (!rec || !rec->mu) {
        return;
    }
    /* 一次 MPD 请求里连续推进有限个 tick（ZCS_SEGMENT_REC_SERVE_MAX_TICKS），
     * 让“还没分片”的起始阶段能较快产出第一片；但不能无限推进，否则一个慢
     * 客户端会把整个 poller 拖在这里。 */
    zb_mutex_lock(rec->mu);
    if (!rec->closing) {
        for (i = 0; i < ZCS_SEGMENT_REC_SERVE_MAX_TICKS; ++i) {
            dash_segmenter_tick_nolock(rec);
        }
    }
    zb_mutex_unlock(rec->mu);
}

/*
 * 决定本路流走 zm packager 还是 legacy 内建路径（一次性，成功或失败都只尝试一次）。
 *
 * 选择依据是 codec 组合：zm packager 只吃 H.264/H.265/H.266 + AAC；
 * 其它组合（VP8/VP9/AV1/Opus）落到 legacy —— 后者按 codec 自行构造
 * adaptation set 与 fMP4 box。这是一个“能力探测”，不是错误。
 */
static void dash_ensure_zm_output(zcs_http_dash_segmenter *rec)
{
    zm_track tracks[4];
    size_t tn = 0;
    size_t i;

    if (!rec || rec->zm_pkg || rec->zm_tried || !rec->src) {
        return;
    }
    /* zm_tried 一次性门闩：无论成功与否只尝试一次。track 组合不满足（视频非
     * H264/H265 或音频非 AAC）则 goto fallback，永不使用 zm packager，后续由
     * legacy dash_mpd 处理。 */
    if (zcs_zm_tracks_from_source(rec->src, tracks, 4, &tn) != ZM_OK || tn == 0) {
        return;
    }
    rec->zm_tried = 1;

    /* packager 的 fMP4 转换覆盖 H.264/H.265/H.266 + AAC；其他组合走 legacy。 */
    for (i = 0; i < tn; ++i) {
        if (tracks[i].kind == ZM_KIND_VIDEO &&
            tracks[i].codec != ZM_CODEC_H264 && tracks[i].codec != ZM_CODEC_H265) {
            goto fallback;
        }
        if (tracks[i].kind == ZM_KIND_AUDIO && tracks[i].codec != ZM_CODEC_AAC) {
            goto fallback;
        }
    }
    {
        zm_packager_cfg pcfg;
        zm_packager_cb pcb;

        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.segment_ms = rec->seg_ms > 0 ? rec->seg_ms : 2000;
        pcfg.live = 1;
        pcfg.prefix = rec->src->stream;
        memset(&pcb, 0, sizeof(pcb));
        pcb.on_init = dash_zm_on_init;
        pcb.on_segment = dash_zm_on_segment;
        pcb.on_playlist = dash_zm_on_playlist;
        rec->zm_pkg = zm_dash_packager_open(tracks, tn, &pcfg, &pcb, rec);
        if (rec->zm_pkg) {
            zn_info("DASH live: zm packager active (%s/%s)", rec->src->app, rec->src->stream);
        }
    }
fallback:
    for (i = 0; i < tn; ++i) {
        zm_track_clear(&tracks[i]);
    }
}

static void dash_segmenter_tick_nolock(zcs_http_dash_segmenter *rec)
{
    zcs_gop_slot slot;

    if (!rec) {
        return;
    }
    if (rec->closing || !rec->src || !rec->reader || !rec->mpd) {
        return;
    }
    /* 首选 zm packager 路径：一旦建成，后续所有帧都只喂给 zm_packager_write，
     * 不再走 legacy 的 dash_mpd 逻辑。 */
    dash_ensure_zm_output(rec);
    if (rec->zm_pkg) {
        for (int n = 0;
             n < ZCS_SEGMENT_REC_TICK_FRAMES && zcs_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
             ++n) {
            zm_packet pkt;
            if (!slot.data || slot.len == 0 || slot.config_frame) {
                continue;
            }
            if (slot.track == ZCS_TRACK_VIDEO) {
                zcs_gop_slot_refresh_play_key(&slot);
            }
            /* 时间戳先经 A/V timeline 换算成相对原点，再交给 packager 切段，
             * 保证音视频对齐。 */
            zcs_zm_packet_from_gop_slot(&slot, &pkt);
            pkt.dts_ms = (int64_t)dash_mux_dts_ms(rec, slot.track, slot.dts_ms);
            pkt.pts_ms = pkt.dts_ms;
            (void)zm_packager_write(rec->zm_pkg, &pkt);
        }
        return;
    }
    /* legacy 路径：先补发 video/audio config（建 adaptation set），再逐帧喂
     * dash_mpd。config 尚未拿到时回落到用源上报的 codec 预判。 */
    if (!rec->sent_video_cfg || rec->adapt_video < 0) {
        size_t clen = 0;
        const uint8_t *cfg = zcs_gop_queue_video_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            dash_feed_video_cfg(rec, cfg, clen);
            if (rec->adapt_video >= 0) {
                rec->sent_video_cfg = 1;
            }
        } else if (rec->src && rec->src->video.ready &&
                   rec->src->video.codec != ZCS_CODEC_INVALID) {
            rec->video_codec = rec->src->video.codec;
        }
    }
    if (rec->enable_audio && (!rec->sent_audio_cfg || rec->adapt_audio < 0)) {
        size_t clen = 0;
        const uint8_t *cfg = zcs_gop_queue_audio_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            dash_feed_audio_cfg(rec, cfg, clen);
            if (rec->adapt_audio >= 0) {
                rec->sent_audio_cfg = 1;
            }
        } else if (rec->src && rec->src->audio.ready && rec->src->audio.codec == ZCS_CODEC_OPUS) {
            dash_ensure_opus_adaptation(rec);
        }
    }
    for (int n = 0;
         n < ZCS_SEGMENT_REC_TICK_FRAMES && zcs_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
         ++n) {
        if (!slot.data || slot.len == 0 || slot.config_frame) {
            continue;
        }
        if (slot.track == ZCS_TRACK_VIDEO) {
            zcs_gop_slot_refresh_play_key(&slot);
            if (slot.codec == ZCS_CODEC_H265) {
                dash_feed_h265(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_H264) {
                dash_feed_h264(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_VP8 || slot.codec == ZCS_CODEC_VP9) {
                dash_feed_vpx(rec, slot.codec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_AV1) {
                dash_feed_av1(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            }
        } else if (slot.track == ZCS_TRACK_AUDIO) {
            if (slot.codec == ZCS_CODEC_OPUS) {
                dash_feed_opus(rec, slot.data, slot.len, slot.dts_ms);
            } else {
                dash_feed_aac(rec, slot.data, slot.len, slot.dts_ms);
            }
        }
    }
}

void zcs_http_dash_segmenter_tick(zcs_http_dash_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (!rec->closing) {
        dash_segmenter_tick_nolock(rec);
    }
    zb_mutex_unlock(rec->mu);
}

zn_err_t zcs_http_dash_segmenter_serve_mpd(zcs_http_dash_segmenter *rec, char *out, size_t cap,
                                            size_t *out_len, int max_ticks, int *has_media)
{
    zcs_http_dash_playlist *maker;
    zn_err_t err;

    (void)max_ticks;
    if (has_media) {
        *has_media = 0;
    }
    if (!rec || !rec->mu || !out || !out_len) {
        return ZN_ERR_INVALID;
    }
    maker = rec->maker;
    if (!maker) {
        return ZN_ERR_INVALID;
    }

    /* 已有分片时勿在每次 MPD 轮询时 pump：ffplay 高频刷新会抽空 reader，导致 MPD 停滞 */
    if (!zcs_http_dash_playlist_has_media_segment(maker)) {
        dash_segmenter_serve_pump_mpd(rec);
    }

    if (!zcs_http_dash_playlist_has_media_segment(maker)) {
        return ZN_ERR_INVALID;
    }

    if (rec->mu) {
        zb_mutex_lock(rec->mu);
    }
    /* 有 zm packager 产出的 MPD 时优先用它；否则让 legacy playlist 现场构建
     * 一份。两者都放在锁内确认 maker 仍有效。 */
    if (rec->zm_mpd_len > 0) {
        size_t n = rec->zm_mpd_len < cap - 1 ? rec->zm_mpd_len : cap - 1;
        memcpy(out, rec->zm_mpd, n);
        out[n] = '\0';
        *out_len = n;
        if (has_media) {
            *has_media = 1;
        }
        if (rec->mu) {
            zb_mutex_unlock(rec->mu);
        }
        return ZN_OK;
    }
    if (rec->mu) {
        zb_mutex_unlock(rec->mu);
    }

    err = zcs_http_dash_playlist_build_mpd(maker, out, cap, out_len);
    if (err == ZN_OK && has_media) {
        *has_media = 1;
    }
    return err;
}

/*
 * 取一个分片并**拷贝**到调用方缓冲。
 *
 * 供小包/单次发送的场景使用（如 HTTP 小响应）。大分片应优先用
 * ref_segment：拷贝一份 MB 级分片会带来无谓的内存带宽开销。
 *
 * max_ticks 是“分片尚未就绪时最多推进几次切片”的上限，避免 HTTP 线程
 * 无限期停留在推进循环里。
 */
zn_err_t zcs_http_dash_segmenter_copy_segment(zcs_http_dash_segmenter *rec, const char *name,
                                               uint8_t *buf, size_t cap, size_t *out_len,
                                               int max_ticks)
{
    zcs_http_dash_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    maker = rec->maker;
    if (!maker) {
        return ZN_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZCS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    if (zcs_http_dash_playlist_copy_segment(maker, name, buf, cap, out_len) == ZN_OK &&
        *out_len > 0) {
        return ZN_OK;
    }

    /* 请求的分片还没生成：边推进边重试，最多 max_ticks 次。每次 tick 前后都
     * 重新持锁，避免长时间占锁（tick 内部自己会加锁，读 reader 可能较慢）。
     * closing 时立即放弃，防止对已销毁对象继续操作。 */
    for (i = 0; i < max_ticks; ++i) {
        zb_mutex_lock(rec->mu);
        if (rec->closing) {
            zb_mutex_unlock(rec->mu);
            return ZN_ERR_INVALID;
        }
        dash_segmenter_tick_nolock(rec);
        zb_mutex_unlock(rec->mu);
        if (zcs_http_dash_playlist_copy_segment(maker, name, buf, cap, out_len) == ZN_OK &&
            *out_len > 0) {
            return ZN_OK;
        }
    }
    *out_len = 0;
    return ZN_ERR_INVALID;
}

/*
 * 取一个分片并**引用**（不拷贝），由调用方负责 zn_buf_unref。
 *
 * 大分片首选这个接口：拿的是内部 zn_buf 的引用，省掉一次 memcpy。
 * 分片可能因环形缓冲淘汰而短暂不存在，因此内部会按 max_ticks 推进重试。
 */
zn_err_t zcs_http_dash_segmenter_ref_segment(zcs_http_dash_segmenter *rec, const char *name,
                                              zn_buf **out_buf, size_t *out_len, int max_ticks)
{
    zcs_http_dash_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !out_buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    maker = rec->maker;
    if (!maker) {
        return ZN_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZCS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    if (zcs_http_dash_playlist_ref_segment(maker, name, out_buf, out_len) == ZN_OK &&
        *out_len > 0) {
        return ZN_OK;
    }

    for (i = 0; i < max_ticks; ++i) {
        zb_mutex_lock(rec->mu);
        if (rec->closing) {
            zb_mutex_unlock(rec->mu);
            return ZN_ERR_INVALID;
        }
        dash_segmenter_tick_nolock(rec);
        zb_mutex_unlock(rec->mu);
        if (zcs_http_dash_playlist_ref_segment(maker, name, out_buf, out_len) == ZN_OK &&
            *out_len > 0) {
            return ZN_OK;
        }
    }
    *out_len = 0;
    return ZN_ERR_INVALID;
}

static zn_err_t dash_ops_create_live(zcs_media_source *src, const void *opts, void **out_rec)
{
    zcs_http_dash_segmenter *rec;

    if (!out_rec) {
        return ZN_ERR_INVALID;
    }
    rec = zcs_http_dash_segmenter_create(src, (const zcs_http_dash_segmenter_opts *)opts);
    if (!rec) {
        return ZN_ERR_INVALID;
    }
    *out_rec = rec;
    return ZN_OK;
}

static void dash_ops_destroy(void *rec)
{
    zcs_http_dash_segmenter_destroy((zcs_http_dash_segmenter *)rec);
}

static void dash_ops_bind_timer(void *rec, zn_poller *poller)
{
    zcs_http_dash_segmenter_bind_timer((zcs_http_dash_segmenter *)rec, poller);
}

static void dash_ops_tick(void *rec)
{
    zcs_http_dash_segmenter_tick((zcs_http_dash_segmenter *)rec);
}

static void dash_ops_touch(void *rec)
{
    zcs_http_dash_segmenter_touch((zcs_http_dash_segmenter *)rec);
}

const zcs_segment_recorder_ops *zcs_http_dash_segment_recorder_ops(void)
{
    static const zcs_segment_recorder_ops k_dash_ops = {
        .name = ZCS_SEGMENT_REC_DASH,
        .create_live = dash_ops_create_live,
        .destroy = dash_ops_destroy,
        .bind_timer = dash_ops_bind_timer,
        .tick = dash_ops_tick,
        .touch = dash_ops_touch,
    };
    return &k_dash_ops;
}
