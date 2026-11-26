/**
 * @file fmp4_writer.c
 * @brief HLS 录制 fMP4（CMAF）mux 路径：轨道设置与按 codec 喂入。
 *        覆盖 VP8/VP9/AV1 视频与 AAC/Opus 音频；从 http_hls_segmenter.c 拆出，
 *        由 recorder tick 循环调度。
 *
 * Copyright (c) zero-media-server
 */
#include "play/hls/segmenter_internal.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/av1/av1_over_rtmp.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/h265/h265_over_rtmp.h"
#include "zms/media/codec/bitstream/annexb.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "webm-vpx.h"
#include "zn/util/buf.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

/*
 * fMP4 分片回调：复用 TS 的 hls_media_segment_cb 入 playlist 后，额外把
 * “下一段起点” fmp4_seg_origin_dts_ms 推到本段末尾。
 *
 * 用 max(end, 上段末尾) 单调钳制：分片时长由 muxer 估算，偶尔会比上一层
 * 记录的末尾更小（B 帧/时长抖动），若不取 max，下一段的切片阈值会倒退，
 * 导致按键帧判断处提前重切或空切。
 */
int hls_fmp4_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms, int64_t dts_ms,
                        int64_t duration_ms)
{
    zms_http_hls_segmenter *rec = (zms_http_hls_segmenter *)param;
    int r;

    (void)pts_ms;
    r = hls_media_segment_cb(param, data, bytes, pts_ms, dts_ms, duration_ms);
    if (r == 0 && rec) {
        uint64_t end = (uint64_t)dts_ms + (uint64_t)(duration_ms > 0 ? duration_ms : 0);

        if (end < rec->last_mux_dts_ms) {
            end = rec->last_mux_dts_ms;
        }
        rec->fmp4_seg_origin_dts_ms = (uint32_t)end;
    }
    return r;
}

static int hls_fmp4_seg_duration_ms(const zms_http_hls_segmenter *rec)
{
    int64_t ms;

    if (!rec) {
        return 2000;
    }
    ms = (int64_t)(rec->seg_duration_sec * 1000.f);
    return ms > 0 ? (int)ms : 2000;
}

static void hls_fmp4_flush_at(zms_http_hls_segmenter *rec, uint32_t dts_ms)
{
    if (!rec || !rec->fmp4_mux || !rec->video_armed || rec->fmp4_video_track < 0) {
        return;
    }
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_video_track, NULL, 0, (int64_t)dts_ms,
                                   (int64_t)dts_ms, ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE);
    rec->fmp4_seg_origin_dts_ms = dts_ms;
}

/* 只在关键帧处切段：fMP4/CMAF 要求每个 segment 以 IDR/sync 起始，否则独立
 * 解码不了。持续累计到超过目标时长后，遇到下一个关键帧才真正落盘。 */
static void hls_fmp4_maybe_flush_on_key(zms_http_hls_segmenter *rec, uint32_t rel_dts_ms,
                                        int keyframe)
{
    uint32_t dur_ms;

    if (!rec || !keyframe) {
        return;
    }
    dur_ms = (uint32_t)hls_fmp4_seg_duration_ms(rec);
    if (rel_dts_ms < rec->fmp4_seg_origin_dts_ms + dur_ms) {
        return;
    }
    hls_fmp4_flush_at(rec, rel_dts_ms);
}

/** 首个 m3u8 请求时强制落盘，避免 live 起播等满 2s 时间轴才出首片。 */
void hls_fmp4_serve_flush(zms_http_hls_segmenter *rec)
{
    if (!rec || !rec->fmp4_mux || !rec->video_armed || rec->fmp4_video_track < 0 || !rec->maker) {
        return;
    }
    if (zms_http_hls_playlist_segment_count(rec->maker) > 0) {
        return;
    }
    if ((uint32_t)rec->last_mux_dts_ms <= rec->fmp4_seg_origin_dts_ms) {
        return;
    }
    hls_fmp4_flush_at(rec, (uint32_t)rec->last_mux_dts_ms);
}

/* 构造 Opus 的 Identification Header（"OpusHead"，RFC 7845）。
 * fMP4 的 Opus 音轨需要在 init 里带这段 extradata，否则播放器不知道声道
 * 映射与预跳采样数。这里填最小可用版本：version=1、预跳 0x0138(312)、
 * 输入采样率按实际（缺省 48k）。 */
static void hls_fmp4_build_opus_extra(int channels, int sample_rate, uint8_t *out, size_t cap,
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

static int hls_aac_is_adts(const uint8_t *es, size_t es_len)
{
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

/* 单个 AAC 帧（1024 采样）换算成毫秒时长，向上取整。
 * 用于给不带时间戳的 ADTS 多帧流估算每帧的 ats 递增量（见 feed_aac_au）。 */
static uint32_t hls_fmp4_aac_au_duration_ms(const zms_http_hls_segmenter *rec)
{
    int sr;

    if (!rec || !rec->have_aac) {
        return 23u;
    }
    sr = zms_aac_config_sample_rate(&rec->aac);
    if (sr <= 0) {
        sr = 48000;
    }
    return (1024u * 1000u + (uint32_t)sr - 1u) / (uint32_t)sr;
}

/*
 * 生成并登记 init segment（CMAF moov）。必须等所有轨道都就绪才生成：
 * init 里要一次性写明全部 track 的编码参数，若在音频轨尚未建立时先写，
 * 后续音频帧就没有对应的 track 描述，播放器会丢音或拒播。所以当源声明了
 * 音频但音频轨还没建好时，这里直接 return，等下一个能触发 maybe_init 的
 * 帧（音轨建立后会再次调用）再生成。fmp4_init_done 保证只发一次。
 */
static void hls_fmp4_maybe_init(zms_http_hls_segmenter *rec)
{
    uint8_t init[8192];
    int init_len;
    zn_buf *buf;

    if (!rec || rec->fmp4_init_done || !rec->fmp4_mux || !rec->maker || rec->fmp4_video_track < 0) {
        return;
    }
    if (rec->enable_audio && rec->fmp4_audio_track < 0 && rec->src && rec->src->has_audio &&
        rec->src->audio.ready &&
        (rec->src->audio.codec == ZMS_CODEC_OPUS || rec->src->audio.codec == ZMS_CODEC_AAC)) {
        return;
    }
    init_len = zms_hls_fmp4_init_segment(rec->fmp4_mux, init, sizeof(init));
    if (init_len <= 0) {
        return;
    }
    buf = zn_buf_alloc((size_t)init_len);
    if (!buf) {
        return;
    }
    memcpy((void *)zn_buf_data(buf), init, (size_t)init_len);
    zn_buf_set_len(buf, (size_t)init_len);
    if (zms_http_hls_playlist_store_init_segment(rec->maker, buf) != ZN_OK) {
        zn_buf_unref(buf);
        return;
    }
    if (rec->save_enabled) {
        hls_save_write_file(rec, "init.mp4", init, (size_t)init_len);
    }
    rec->fmp4_init_done = 1;
    zn_info("HLS recorder: fMP4 init segment ready len=%d", init_len);
}

/* 由首个关键帧生成 VP8/VP9 的 codec configuration record（vpcC），作为 fMP4
 * 视频轨的 extradata。WebM/VPx 没有像 avcC 那样的现成记录，需从关键帧头解析
 * 出宽高与 profile。拿不到宽高时用 640x480 兜底只为占位，真实分辨率通常能从
 * 头里解析出来。 */
static void hls_fmp4_ensure_vpx_track(zms_http_hls_segmenter *rec, zms_codec_id vc,
                                      const uint8_t *keyframe, size_t len)
{
    uint8_t vpxc[32];
    struct webm_vpx_t vpx;
    int w = 0;
    int h = 0;
    int n;

    if (!rec || !rec->fmp4_mux || rec->fmp4_video_track >= 0 || !keyframe || len == 0) {
        return;
    }
    if (vc != ZMS_CODEC_VP8 && vc != ZMS_CODEC_VP9) {
        return;
    }
    memset(&vpx, 0, sizeof(vpx));
    if (vc == ZMS_CODEC_VP9) {
        if (webm_vpx_codec_configuration_record_from_vp9(&vpx, &w, &h, keyframe, len) < 0) {
            return;
        }
    } else if (webm_vpx_codec_configuration_record_from_vp8(&vpx, &w, &h, keyframe, len) < 0) {
        return;
    }
    n = webm_vpx_codec_configuration_record_save(&vpx, vpxc, sizeof(vpxc));
    if (n <= 0) {
        return;
    }
    rec->fmp4_video_track = zms_hls_fmp4_add_video(rec->fmp4_mux, vc, w > 0 ? w : 640,
                                                   h > 0 ? h : 480, vpxc, (size_t)n);
    if (rec->fmp4_video_track >= 0) {
        rec->video_codec = vc;
        rec->sent_video_cfg = 1;
        zn_info("HLS recorder: %s fMP4 video track ready", zms_codec_name(vc));
        hls_fmp4_maybe_init(rec);
    }
}

/* AV1 轨：从首个 OBU 里提取 av1C（含 sequence header），同样以关键帧为触发点。
 * av1c 缓冲给 2KB 足够容纳 AV1 的序列头配置。 */
static void hls_fmp4_ensure_av1_track(zms_http_hls_segmenter *rec, const uint8_t *obu, size_t len)
{
    uint8_t av1c[2048];
    int w = 0;
    int h = 0;
    int n;

    if (!rec || !rec->fmp4_mux || rec->fmp4_video_track >= 0 || !obu || len == 0) {
        return;
    }
    n = zms_av1_extradata_from_obu(obu, len, av1c, sizeof(av1c), &w, &h);
    if (n <= 0) {
        return;
    }
    rec->fmp4_video_track = zms_hls_fmp4_add_video(rec->fmp4_mux, ZMS_CODEC_AV1, w > 0 ? w : 640,
                                                   h > 0 ? h : 480, av1c, (size_t)n);
    if (rec->fmp4_video_track >= 0) {
        rec->video_codec = ZMS_CODEC_AV1;
        rec->sent_video_cfg = 1;
        zn_info("HLS recorder: AV1 fMP4 video track ready");
        hls_fmp4_maybe_init(rec);
    }
}

void hls_fmp4_ensure_aac_track(zms_http_hls_segmenter *rec, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;
    int sr = 44100;
    int ch = 2;
    int track;

    if (!rec || !rec->fmp4_mux || rec->fmp4_audio_track >= 0 || !cfg || clen < 4) {
        return;
    }
    if (zms_flv_tag_audio_codec(cfg, clen) != ZMS_CODEC_AAC) {
        return;
    }
    if (!zms_rtmp_aac_extradata(cfg, clen, &asc, &asc_len) || !asc || asc_len == 0) {
        return;
    }
    zms_aac_config_set_defaults(&rec->aac, 44100, 2);
    if (!zms_aac_config_load_asc(&rec->aac, asc, asc_len)) {
        return;
    }
    rec->have_aac = 1;
    sr = zms_aac_config_sample_rate(&rec->aac);
    if (sr <= 0) {
        sr = rec->aac_sr > 0 ? rec->aac_sr : 48000;
    }
    ch = zms_aac_config_channels(&rec->aac);
    if (ch <= 0) {
        ch = 2;
    }
    track = zms_hls_fmp4_add_audio(rec->fmp4_mux, ZMS_CODEC_AAC, ch, 16, sr, asc, asc_len);
    if (track >= 0) {
        rec->fmp4_audio_track = track;
        rec->sent_audio_cfg = 1;
        zn_info("HLS recorder: AAC fMP4 audio track ready rate=%d ch=%d", sr, ch);
        hls_fmp4_maybe_init(rec);
    }
}

void hls_fmp4_ensure_opus_track(zms_http_hls_segmenter *rec)
{
    uint8_t opus_head[32];
    size_t head_len = 0;
    int ch = 2;
    int sr = 48000;

    if (!rec || !rec->fmp4_mux || rec->fmp4_audio_track >= 0 || !rec->enable_audio) {
        return;
    }
    if (rec->src && rec->src->audio.ready) {
        ch = rec->src->audio.channels > 0 ? rec->src->audio.channels : 2;
        sr = rec->src->audio.sample_rate > 0 ? rec->src->audio.sample_rate : 48000;
    }
    hls_fmp4_build_opus_extra(ch, sr, opus_head, sizeof(opus_head), &head_len);
    if (head_len == 0) {
        return;
    }
    rec->fmp4_audio_track =
        zms_hls_fmp4_add_audio(rec->fmp4_mux, ZMS_CODEC_OPUS, ch, 16, sr, opus_head, head_len);
    if (rec->fmp4_audio_track >= 0) {
        rec->sent_audio_cfg = 1;
        zn_info("HLS recorder: Opus fMP4 audio track ready rate=%d ch=%d", sr, ch);
        hls_fmp4_maybe_init(rec);
    }
}

void hls_fmp4_feed_vpx(zms_http_hls_segmenter *rec, zms_codec_id vc, const uint8_t *es, size_t len,
                       uint32_t dts_ms, int keyframe)
{
    uint32_t rel_dts_ms;
    int flags;

    if (!rec || !rec->fmp4_mux || !es || len == 0) {
        return;
    }
    if (keyframe) {
        hls_fmp4_ensure_vpx_track(rec, vc, es, len);
    }
    if (rec->fmp4_video_track < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        /* 起始前必须先在关键帧上“上膛”：锁定时间轴原点，并记录首段起点。
         * 之前到达的非关键帧一律丢，否则首个分片没有可解码起点。 */
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->fmp4_seg_origin_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
        zn_info("HLS recorder: first %s key dts_ms=%u (fMP4)", zms_codec_name(vc),
                 (unsigned)dts_ms);
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    rec->last_video_dts_ms = rel_dts_ms;
    flags = ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE;
    if (keyframe) {
        flags |= ZMS_HLS_FMP4_FLAG_KEYFRAME;
    }
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_video_track, es, len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms, flags);
    hls_fmp4_maybe_flush_on_key(rec, rel_dts_ms, keyframe);
}

static uint8_t *fmp4_scratch(zms_http_hls_segmenter *rec, size_t need)
{
    uint8_t *nb;

    if (!rec) {
        return NULL;
    }
    if (rec->mux_buf && rec->mux_buf_cap >= need) {
        return rec->mux_buf;
    }
    nb = (uint8_t *)realloc(rec->mux_buf, need);
    if (!nb) {
        return NULL;
    }
    rec->mux_buf = nb;
    rec->mux_buf_cap = need;
    return nb;
}

/** Annex-B AU → 4 字节长度前缀样本（fMP4 输入格式）。 */
/*
 * ISO-BMFF/CMAF 的 NAL 存储不用起始码，而是每个 NAL 前置大端长度字段。
 * 这里固定 4 字节前缀（prefer_4byte 两个分支取值都是 4，保留参数仅为接口
 * 对称/未来兼容），逐 NAL 转换；容量不足返回 -1，由调用方丢弃该帧。
 */
static int fmp4_annexb_to_length_prefixed(const uint8_t *in, size_t len, uint8_t *out,
                                          size_t cap, int prefer_4byte)
{
    const uint8_t *p = in;
    const uint8_t *end = in + len;
    size_t used = 0;
    int prefix = prefer_4byte ? 4 : 4;

    while (p < end) {
        size_t nl = 0;
        const uint8_t *nal = zms_annexb_find_nal(p, end, &nl);

        if (!nal || nl == 0) {
            break;
        }
        if (used + (size_t)prefix + nl > cap) {
            return -1;
        }
        if (prefix == 4) {
            out[used++] = (uint8_t)(nl >> 24);
            out[used++] = (uint8_t)(nl >> 16);
            out[used++] = (uint8_t)(nl >> 8);
            out[used++] = (uint8_t)nl;
        } else {
            out[used++] = (uint8_t)(nl >> 8);
            out[used++] = (uint8_t)nl;
        }
        memcpy(out + used, nal, nl);
        used += nl;
        p = nal + nl;
    }
    return (int)used;
}

/* H.264/H.265 视频轨：extradata 取自源已缓存的 FLV video config（avcC/hvcC），
 * 与新录制的 mp4 recorder 保持一致。仅关键帧触发建轨，因为此时 config 必然
 * 已随流到达。 */
static void hls_fmp4_ensure_h26x_track(zms_http_hls_segmenter *rec, zms_codec_id codec,
                                       int keyframe)
{
    const uint8_t *cfg;
    const uint8_t *extra = NULL;
    size_t clen = 0;
    size_t extra_len = 0;
    int w;
    int h;

    if (!rec || !rec->fmp4_mux || rec->fmp4_video_track >= 0 || !keyframe || !rec->src ||
        !rec->src->gop_queue) {
        return;
    }
    cfg = zms_gop_queue_video_config(rec->src->gop_queue, &clen);
    if (!cfg || clen == 0) {
        return;
    }
    if (codec == ZMS_CODEC_H264) {
        if (!zms_rtmp_avc_extradata(cfg, clen, &extra, &extra_len) || !extra || extra_len == 0) {
            return;
        }
    } else if (codec == ZMS_CODEC_H265) {
        if (!zms_rtmp_hevc_extradata(cfg, clen, &extra, &extra_len) || !extra ||
            extra_len == 0) {
            return;
        }
    } else {
        return;
    }
    w = rec->src->video.width > 0 ? (int)rec->src->video.width : 640;
    h = rec->src->video.height > 0 ? (int)rec->src->video.height : 480;
    rec->fmp4_video_track =
        zms_hls_fmp4_add_video(rec->fmp4_mux, codec, w, h, extra, extra_len);
    if (rec->fmp4_video_track >= 0) {
        rec->video_codec = codec;
        rec->sent_video_cfg = 1;
        zn_info("HLS recorder: %s fMP4 video track ready %dx%d", zms_codec_name(codec), w, h);
        hls_fmp4_maybe_init(rec);
    }
}

void hls_fmp4_feed_h26x(zms_http_hls_segmenter *rec, zms_codec_id codec, const uint8_t *annexb,
                        size_t len, uint32_t dts_ms, int keyframe)
{
    uint32_t rel_dts_ms;
    uint8_t *scratch;
    int n;
    int flags;

    if (!rec || !rec->fmp4_mux || !annexb || len == 0) {
        return;
    }
    if (keyframe) {
        hls_fmp4_ensure_h26x_track(rec, codec, 1);
    }
    if (rec->fmp4_video_track < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->fmp4_seg_origin_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
        zn_info("HLS recorder: first %s key dts_ms=%u (fMP4)", zms_codec_name(codec),
                 (unsigned)dts_ms);
    }
    scratch = fmp4_scratch(rec, len + 4 * 8 + 16);
    if (!scratch) {
        return;
    }
    n = fmp4_annexb_to_length_prefixed(annexb, len, scratch, rec->mux_buf_cap, 1);
    if (n <= 0) {
        return;
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    rec->last_video_dts_ms = rel_dts_ms;
    flags = ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE;
    if (keyframe) {
        flags |= ZMS_HLS_FMP4_FLAG_KEYFRAME;
    }
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_video_track, scratch, (size_t)n,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms, flags);
    hls_fmp4_maybe_flush_on_key(rec, rel_dts_ms, keyframe);
}

typedef struct {
    zms_http_hls_segmenter *rec;
    uint32_t base_dts_ms;
    unsigned idx;
    uint32_t au_dur_ms;
} hls_fmp4_aac_ctx;

static int hls_fmp4_feed_aac_au(const uint8_t *au, size_t len, void *user)
{
    hls_fmp4_aac_ctx *ctx = (hls_fmp4_aac_ctx *)user;
    uint32_t rel_dts_ms;
    const uint8_t *raw = au;
    size_t raw_len = len;
    uint32_t ats;

    if (!ctx || !ctx->rec || !au || len == 0) {
        return -1;
    }
    if (zms_aac_es_to_raw(au, len, &raw, &raw_len) != ZN_OK || !raw || raw_len == 0) {
        return -1;
    }
    ats = ctx->base_dts_ms + ctx->idx * ctx->au_dur_ms;
    /* 音频不进 GOP 队列、时间戳靠估算，一旦估算漂移到视频前面太多（>4s），
     * 说明这批音频与当前视频时间轴已脱节，直接丢弃并不再推进，避免在
     * fMP4 里写入远超视频轨的音频时间戳把 segment 时长带偏。 */
    if (ctx->rec->last_video_dts_ms > 0 && ats > ctx->rec->last_video_dts_ms + 4000u) {
        return 0;
    }
    rel_dts_ms = hls_mux_dts_ms(ctx->rec, ZMS_TRACK_AUDIO, ats);
    ctx->rec->last_mux_dts_ms = rel_dts_ms;
    (void)zms_hls_fmp4_write_frame(ctx->rec->fmp4_mux, ctx->rec->fmp4_audio_track, raw, raw_len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms,
                                   ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE);
    ctx->idx++;
    return 0;
}

void hls_fmp4_feed_aac(zms_http_hls_segmenter *rec, const uint8_t *es, size_t es_len,
                       uint32_t dts_ms)
{
    hls_fmp4_aac_ctx ctx;

    if (!rec || !rec->fmp4_mux || rec->fmp4_audio_track < 0 || !rec->enable_audio ||
        !rec->video_armed || !es || es_len == 0) {
        return;
    }
    if (!rec->have_aac) {
        return;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.rec = rec;
    ctx.base_dts_ms = dts_ms;
    ctx.au_dur_ms = hls_fmp4_aac_au_duration_ms(rec);
    if (hls_aac_is_adts(es, es_len)) {
        /* ADTS 一个 buf 里可能串了多帧、且不带逐帧时间戳，交给 foreach 拆帧，
         * 每帧按 1024 采样步进估算时间戳（见 feed_aac_au）。 */
        (void)zms_aac_es_foreach_frame(es, es_len, hls_fmp4_feed_aac_au, &ctx);
    } else {
        (void)hls_fmp4_feed_aac_au(es, es_len, &ctx);
    }
}

void hls_fmp4_feed_av1(zms_http_hls_segmenter *rec, const uint8_t *es, size_t len, uint32_t dts_ms,
                       int keyframe)
{
    uint32_t rel_dts_ms;
    int flags;

    if (!rec || !rec->fmp4_mux || !es || len == 0) {
        return;
    }
    if (!keyframe) {
        zms_gop_slot slot;
        memset(&slot, 0, sizeof(slot));
        slot.codec = ZMS_CODEC_AV1;
        slot.track = ZMS_TRACK_VIDEO;
        slot.data = (uint8_t *)es;
        slot.len = len;
        slot.keyframe = keyframe;
        zms_gop_slot_refresh_play_key(&slot);
        keyframe = slot.keyframe;
    }
    if (keyframe) {
        hls_fmp4_ensure_av1_track(rec, es, len);
    }
    if (rec->fmp4_video_track < 0) {
        return;
    }
    if (!rec->video_armed) {
        if (!keyframe) {
            return;
        }
        rec->video_armed = 1;
        zms_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->fmp4_seg_origin_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
        zn_info("HLS recorder: first AV1 key dts_ms=%u (fMP4)", (unsigned)dts_ms);
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_VIDEO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    rec->last_video_dts_ms = rel_dts_ms;
    flags = ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE;
    if (keyframe) {
        flags |= ZMS_HLS_FMP4_FLAG_KEYFRAME;
    }
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_video_track, es, len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms, flags);
    hls_fmp4_maybe_flush_on_key(rec, rel_dts_ms, keyframe);
}

/* Opus 音频写入。与视频一样要求 video_armed：先有视频关键帧再写音轨，保证
 * 分片以视频同步起点开始。Opus 包直接是原始包，无需 ADTS 之类的外层。 */
void hls_fmp4_feed_opus(zms_http_hls_segmenter *rec, const uint8_t *es, size_t len, uint32_t dts_ms)
{
    uint32_t rel_dts_ms;

    if (!rec || !rec->fmp4_mux || !rec->enable_audio || !es || len == 0) {
        return;
    }
    hls_fmp4_ensure_opus_track(rec);
    if (rec->fmp4_audio_track < 0 || !rec->video_armed) {
        return;
    }
    rel_dts_ms = hls_mux_dts_ms(rec, ZMS_TRACK_AUDIO, dts_ms);
    rec->last_mux_dts_ms = rel_dts_ms;
    (void)zms_hls_fmp4_write_frame(rec->fmp4_mux, rec->fmp4_audio_track, es, len,
                                   (int64_t)rel_dts_ms, (int64_t)rel_dts_ms,
                                   ZMS_HLS_FMP4_FLAG_SEGMENT_DISABLE);
}
