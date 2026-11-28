/**
 * @file stream_ingest.c
 * @brief 直播推流接入：将 RTMP 等协议帧写入 gop_queue。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/publish/ingest/common/ingest_codec.h"
#include "zcs/media/codec/aac/aac_over_rtmp.h"
#include "zcs/media/codec/aac/aac_over_rtp.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/container/demux_pipeline.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/media/wire/rtmp.h"
#include "zcs/core/event.h"
#include "zcs/media/clock.h"
#include "zcs/core/codec_filter.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/stream_track.h"
#include "zcs/util/buf_pool.h"
#include "publish/ingest/common/ingest_internal.h"
#include "zn/util/buf.h"
#include "zn/platform.h"
#include "zcs/util/log_throttle.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

static uint32_t ingest_linear_ms_pts(zcs_live_ingest *ch, uint32_t raw_ms)
{
    if (ch->av_origin_set && raw_ms >= ch->av_origin_ms) {
        return raw_ms - ch->av_origin_ms;
    }
    return raw_ms;
}

/*
 * 时间戳映射做了按 raw 值记忆化：同一 raw_ms 的重复调用返回上次结果。
 * 原因有二：一是音视频在同一个 demux 批次里可能以相同原始时间戳出现，
 * 直接透传会得到两个一模一样的 pts（合法但不必要地触发下游判断）；二是
 * timeline 换算（尤其带 B 帧重排/夹取）不便宜，复用可省算力。
 * linear_ms 模式走的是“相对首帧的线性差”，绕过 timeline 的平滑/夹取逻辑，
 * 用于要求严格线性的源。
 */
uint32_t live_ingest_video_pts(zcs_live_ingest *ch, uint32_t raw_ms)
{
    if (ch->last_v_valid && ch->last_v_raw_ms == raw_ms) {
        return ch->last_v_pts;
    }
    ch->last_v_raw_ms = raw_ms;
    if (ch->tl.linear_ms) {
        ch->last_v_pts = ingest_linear_ms_pts(ch, raw_ms);
    } else {
        ch->last_v_pts = zcs_media_timeline_video(&ch->tl, raw_ms);
    }
    ch->last_v_valid = 1;
    return ch->last_v_pts;
}

uint32_t live_ingest_audio_pts(zcs_live_ingest *ch, uint32_t raw_ms)
{
    if (ch->last_a_valid && ch->last_a_raw_ms == raw_ms) {
        return ch->last_a_pts;
    }
    ch->last_a_raw_ms = raw_ms;
    if (ch->tl.linear_ms) {
        ch->last_a_pts = ingest_linear_ms_pts(ch, raw_ms);
    } else {
        ch->last_a_pts = zcs_media_timeline_audio(&ch->tl, raw_ms);
    }
    ch->last_a_valid = 1;
    return ch->last_a_pts;
}

uint8_t *live_ingest_work_buf(zcs_live_ingest *ch)
{
    if (!ch) {
        return NULL;
    }
    if (!ingest_slot_resize(ch, &ch->work_buf, &ch->work_cap, ZCS_LIVE_INGEST_WORK_BUF)) {
        return NULL;
    }
    return ch->work_buf;
}

uint8_t *live_ingest_large_buf(zcs_live_ingest *ch, size_t need)
{
    if (!ch || need == 0) {
        return NULL;
    }
    if (!ingest_slot_resize(ch, &ch->large_buf, &ch->large_cap, need)) {
        return NULL;
    }
    return ch->large_buf;
}

void live_ingest_set_video_config(zcs_live_ingest *ch, const void *data, size_t len)
{
    zn_err_t err;

    if (!ch || !ch->source || !ch->source->gop_queue || !data || len == 0) {
        return;
    }
    err = zcs_gop_queue_set_video_config(ch->source->gop_queue, data, len);
    if (err != ZN_OK) {
        zn_warn("ingress: set video config failed err=%d len=%u", (int)err, (unsigned)len);
    }
}

void live_ingest_set_audio_config(zcs_live_ingest *ch, const void *data, size_t len)
{
    if (!ch || !ch->source || !ch->source->gop_queue || !data || len == 0) {
        return;
    }
    (void)zcs_gop_queue_set_audio_config(ch->source->gop_queue, data, len);
}

static void live_ingest_maybe_mute_audio(zcs_live_ingest *ch, uint32_t dts_ms);

/* [protocol] modify_stamp=1：用接收系统时间（相对首帧）覆盖源时间戳，平滑且单调。 */
/*
 * 关键在于“严格单调”：源时间戳可能因多帧同时间戳或回退导致 DTS 不增，
 * 而 GOP 队列/播放端都假定 DTS 单调。这里 out<=last 时强制 +1ms 兜底，
 * 牺牲一点时间精度换取时间轴可推进、不卡死。
 */
static void live_ingest_modify_stamp(zcs_live_ingest *ch, zcs_frame *f)
{
    int64_t now;
    int64_t out;

    if (!ch || !f || ch->opt.modify_stamp != 1) {
        return;
    }
    now = (int64_t)zn_monotonic_ms();
    if (ch->stamp_origin_ms == 0) {
        ch->stamp_origin_ms = now;
    }
    out = now - ch->stamp_origin_ms;
    if (out <= ch->stamp_last_ms) {
        out = ch->stamp_last_ms + 1;
    }
    ch->stamp_last_ms = out;
    f->dts_ms = (uint64_t)out;
    f->pts_ms = (uint64_t)out;
}

/* 复制一份 frame 再写：copy.owned=0 表明 buffer 归调用方所有，GOP 队列只
 * 引用不接管，避免入站 buffer 生命周期与队列混在一起。modify_stamp 只改
 * 副本，不污染调用方的 frame。 */
void live_ingest_write_frame(zcs_live_ingest *ch, const zcs_frame *frame)
{
    if (!ch || !ch->source || !ch->source->gop_queue || !frame || !frame->data ||
        frame->size == 0) {
        return;
    }
    zcs_frame copy = *frame;
    copy.owned = 0;
    live_ingest_modify_stamp(ch, &copy);
    if (zcs_gop_queue_write(ch->source->gop_queue, &copy) == ZN_OK) {
        zcs_media_stats_on_ingress(ch->source, frame->size);
        zcs_media_stats_on_frame(ch->source, frame->track == ZCS_TRACK_VIDEO);
        if (frame->track == ZCS_TRACK_VIDEO) {
            live_ingest_maybe_mute_audio(ch, (uint32_t)frame->dts_ms);
        }
    } else {
        zcs_media_stats_on_drop(ch->source);
    }
}

/*
 * 纯视频流补静音音轨（add_mute_audio）。
 *
 * 为什么不立即补：不少推流端会先发几帧视频再发音轨配置，立刻补静音会导致
 * 真实音轨到达时“音轨已存在”被拒。所以先等 500ms 静默期；期间一旦
 * real_audio 置位就永不补。补的静音按视频时间轴以 ~23ms（1024/44100 采样）
 * 的间隔生成，保证与视频同钟。
 *
 * mute_audio_active 要在 ensure 前置位：ensure 内部会判“是否已有音轨”，
 * 若不先置位，会把自己创建的静音轨当成外来真实音轨而拒绝。
 */
static void live_ingest_maybe_mute_audio(zcs_live_ingest *ch, uint32_t dts_ms)
{
    if (!ch || !ch->opt.add_mute_audio || ch->real_audio || !ch->source) {
        return;
    }
    if (!ch->source->video.ready) {
        return;
    }
    if (!ch->mute_audio_active) {
        /* 给真实音轨一点时间：等 500ms 仍无音频才补静音 */
        if (!ch->mute_wait_started) {
            ch->mute_wait_started = 1;
            ch->mute_wait_ms = (int64_t)dts_ms;
            return;
        }
        if ((int64_t)dts_ms - ch->mute_wait_ms < 500) {
            return;
        }
    }
    if (!ch->mute_audio_active) {
        ch->mute_audio_active = 1; /* 先置位，避免 ensure 被误判为真实音轨 */
        if (zcs_live_ingest_ensure_aac_config(ch, 44100, 2) != ZN_OK) {
            ch->mute_audio_active = 0;
            return;
        }
        ch->mute_audio_idx = -1;
        zn_info("ingress: add mute AAC (44100/2) for video-only stream");
    }
    /* 以视频时间轴为准，约每 23ms（1024/44100）补一帧静音 AAC */
    if (ch->mute_audio_idx < 0 || (int64_t)dts_ms - ch->mute_audio_idx >= 22) {
        ch->mute_audio_idx = (int64_t)dts_ms;
        zcs_live_ingest_inject_mute_aac(ch, dts_ms);
    }
}

/* 带 buffer 的写入：buf 所有权在此转交。若写入失败必须 unref（否则泄漏），
 * 成功则由 GOP 队列接管。modify_stamp 开启时才复制 frame 改时间戳。 */
zn_err_t live_ingest_write_frame_buf(zcs_live_ingest *ch, zn_buf *buf, const zcs_frame *frame)
{
    zn_err_t err;

    if (!ch || !ch->source || !ch->source->gop_queue || !buf || !frame) {
        return ZN_ERR_INVALID;
    }
    if (ch->opt.modify_stamp == 1) {
        zcs_frame fixed = *frame;
        live_ingest_modify_stamp(ch, &fixed);
        err = zcs_gop_queue_write_buf(ch->source->gop_queue, buf, &fixed);
    } else {
        err = zcs_gop_queue_write_buf(ch->source->gop_queue, buf, frame);
    }
    if (err != ZN_OK) {
        zn_buf_unref(buf);
        zcs_media_stats_on_drop(ch->source);
    } else {
        zcs_media_stats_on_ingress(ch->source, frame->size);
        zcs_media_stats_on_frame(ch->source, frame->track == ZCS_TRACK_VIDEO);
        if (frame->track == ZCS_TRACK_VIDEO) {
            live_ingest_maybe_mute_audio(ch, (uint32_t)frame->dts_ms);
        }
    }
    return err;
}

zcs_live_ingest *zcs_live_ingest_create_vhost(const char *vhost, const char *app,
                                              const char *stream, const zcs_protocol_opts *opts)
{
    if (!app || !stream) {
        return NULL;
    }

    zcs_live_ingest *ch = (zcs_live_ingest *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }

    {
        zcs_protocol_opts def;
        const zcs_protocol_opts *o = opts;

        if (!o) {
            zcs_protocol_opts_default(&def);
            o = &def;
        }
        ch->opt.modify_stamp = o->modify_stamp;
        ch->opt.add_mute_audio = o->add_mute_audio;
    }
    if (ch->opt.modify_stamp) {
        zn_info("ingress: modify_stamp=%d (1=system time, 2=relative)", ch->opt.modify_stamp);
    }

    ch->source = zcs_media_source_register_ex(ZCS_SCHEMA_RTMP, vhost, app, stream);
    if (!ch->source) {
        free(ch);
        return NULL;
    }
    ch->owns_source = 0;
    zcs_media_timeline_reset(&ch->tl);
    return ch;
}

zcs_live_ingest *zcs_live_ingest_create(const char *app, const char *stream,
                                        const zcs_protocol_opts *opts)
{
    return zcs_live_ingest_create_vhost(ZCS_DEFAULT_VHOST, app, stream, opts);
}

zcs_live_ingest *zcs_live_ingest_create_publish(const char *app, const char *stream_requested,
                                                const zcs_protocol_opts *opts)
{
    return zcs_live_ingest_create_publish_schema(ZCS_SCHEMA_RTMP, app, stream_requested, opts);
}

zcs_live_ingest *zcs_live_ingest_create_publish_schema_vhost(const char *schema,
                                                             const char *vhost, const char *app,
                                                             const char *stream_requested,
                                                             const zcs_protocol_opts *opts)
{
    const char *reg_schema = schema && schema[0] ? schema : ZCS_SCHEMA_RTMP;

    if (!app || !stream_requested) {
        return NULL;
    }

    zcs_live_ingest *ch = (zcs_live_ingest *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }

    {
        zcs_protocol_opts def;
        const zcs_protocol_opts *o = opts;

        if (!o) {
            zcs_protocol_opts_default(&def);
            o = &def;
        }
        ch->opt.modify_stamp = o->modify_stamp;
        ch->opt.add_mute_audio = o->add_mute_audio;
    }
    if (ch->opt.modify_stamp) {
        zn_info("ingress: modify_stamp=%d (1=system time, 2=relative)", ch->opt.modify_stamp);
    }

    /* create_publish 系列用 register_publish_ex：声明本源是“推流端”，与纯
     * 播放侧的 register_ex 区分开，影响源的生命周期与 linger/去重语义。
     * schema 缺省回落到 RTMP（绝大多数推流是 RTMP）。 */
    ch->source = zcs_media_source_register_publish_ex(reg_schema, vhost, app, stream_requested);
    if (!ch->source) {
        free(ch);
        return NULL;
    }
    ch->owns_source = 0;
    zcs_media_timeline_reset(&ch->tl);
    return ch;
}

zcs_live_ingest *zcs_live_ingest_create_publish_schema(const char *schema, const char *app,
                                                       const char *stream_requested,
                                                       const zcs_protocol_opts *opts)
{
    return zcs_live_ingest_create_publish_schema_vhost(schema, ZCS_DEFAULT_VHOST, app,
                                                       stream_requested, opts);
}

zcs_live_ingest *zcs_live_ingest_create_publish_vhost(const char *vhost, const char *app,
                                                      const char *stream_requested,
                                                      const zcs_protocol_opts *opts)
{
    return zcs_live_ingest_create_publish_schema_vhost(ZCS_SCHEMA_RTMP, vhost, app,
                                                       stream_requested, opts);
}

zcs_live_ingest *zcs_live_ingest_bind(zcs_media_source *src)
{
    if (!src || !src->gop_queue) {
        return NULL;
    }
    zcs_live_ingest *ch = (zcs_live_ingest *)calloc(1, sizeof(*ch));
    if (!ch) {
        return NULL;
    }
    ch->source = src;
    ch->owns_source = 0;
    zcs_protocol_opts_default(&ch->opt);
    zcs_media_timeline_reset(&ch->tl);
    return ch;
}

void zcs_live_ingest_destroy(zcs_live_ingest *ch)
{
    if (!ch) {
        return;
    }
    zcs_live_ingest_h265_hevc_au_reset(ch);
    zcs_demux_pipeline_destroy(ch->rtmp_demux);
    ingest_slot_clear(ch, &ch->work_buf, &ch->work_cap);
    ingest_slot_clear(ch, &ch->large_buf, &ch->large_cap);
    free(ch);
}

void zcs_live_ingest_set_poller(zcs_live_ingest *ch, struct zn_poller *poller)
{
    if (!ch) {
        return;
    }
    ch->io_poller = poller;
}

zcs_media_source *zcs_live_ingest_source(zcs_live_ingest *ch)
{
    return ch ? ch->source : NULL;
}

/* 推流端断开/重连时清空上游状态（下游源元数据保留，由 reset 另行处理）。
 * 必须重置时间轴、origin、last_* 记忆与 demux，否则重连后新流会沿用上一
 * 条流的 raw→pts 记忆，导致时间戳错乱。 */
void zcs_live_ingest_reset_upstream(zcs_live_ingest *ch)
{
    if (!ch) {
        return;
    }
    zcs_live_ingest_h265_hevc_au_reset(ch);
    zcs_demux_pipeline_destroy(ch->rtmp_demux);
    ch->rtmp_demux = NULL;
    ch->have_video_cfg = 0;
    ch->have_audio_cfg = 0;
    ch->gop_vcfg_applied = 0;
    ch->video_cfg_len = 0;
    ch->av_origin_set = 0;
    ch->av_origin_ms = 0;
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
    ch->rtmp_ingress.active = 0;
    ch->rtmp_frame_got = 0;
    ch->tl_last_v_gop_norm = 0;
    ch->tl_last_v_gop_valid = 0;
    zcs_media_timeline_reset(&ch->tl);
}

void zcs_live_ingest_reset(zcs_live_ingest *ch)
{
    if (!ch || !ch->source) {
        return;
    }
    zcs_live_ingest_reset_upstream(ch);
    zcs_media_source_clear(ch->source);
}

void zcs_live_ingest_mark_real_audio(zcs_live_ingest *ch)
{
    if (ch) {
        ch->real_audio = 1;
        ch->mute_audio_active = 0;
    }
}

void zcs_live_ingest_set_audio_codec(zcs_live_ingest *ch, zcs_codec_id codec, uint32_t sample_rate)
{
    if (ch) {
        zcs_media_timeline_set_audio(&ch->tl, codec, sample_rate);
    }
}

void zcs_live_ingest_set_rtp_clocks(zcs_live_ingest *ch, uint32_t video_clock_hz,
                                    zcs_codec_id audio_codec, uint32_t audio_clock_hz)
{
    if (!ch) {
        return;
    }
    zcs_media_timeline_set_video(&ch->tl, ZCS_CODEC_H264, video_clock_hz);
    if (audio_codec != ZCS_CODEC_INVALID) {
        zcs_media_timeline_set_audio(&ch->tl, audio_codec, audio_clock_hz);
    }
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
}

void zcs_live_ingest_set_stamp_max_delta(zcs_live_ingest *ch, uint32_t max_delta_ms)
{
    if (!ch || max_delta_ms == 0) {
        return;
    }
    ch->tl.vst.max_delta_ms = max_delta_ms;
    ch->tl.ast.max_delta_ms = max_delta_ms;
}

void zcs_live_ingest_set_stamp_av_clamp(zcs_live_ingest *ch, int enabled)
{
    if (!ch) {
        return;
    }
    zcs_media_timeline_set_av_clamp(&ch->tl, enabled);
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
}

void zcs_live_ingest_set_timeline_linear_ms(zcs_live_ingest *ch, int on)
{
    if (!ch) {
        return;
    }
    ch->tl.linear_ms = on ? 1 : 0;
    ch->last_v_valid = 0;
    ch->last_a_valid = 0;
    ch->av_origin_set = 0;
    ch->av_origin_ms = 0;
    ch->tl_last_v_gop_norm = 0;
    ch->tl_last_v_gop_valid = 0;
}

void zcs_live_ingest_set_defer_gop_vcfg(zcs_live_ingest *ch, int on)
{
    if (!ch) {
        return;
    }
    ch->defer_gop_vcfg = on ? 1 : 0;
    ch->gop_vcfg_applied = 0;
    ch->video_cfg_len = 0;
}

zn_err_t zcs_live_ingest_input_frame(zcs_live_ingest *ch, const zcs_frame *frame)
{
    return zcs_live_ingest_codec_input_dispatch(ch, frame);
}

/*
 * flv_demux 解出的一帧 → GOP 队列。
 *
 * 拷贝到 gop_queue 分配的新 buf（每次新建，不复用槽位，避免与并发 clear 争用
 * 同一块内存），然后走 write_frame_buf 交所有权。音频特殊处理：demux 出来的 AAC 可能仍带 ADTS
 * 头，而内部统一用 raw AAC(ASC) 存储，这里就地转 raw 并 memmove 回 buf 头部。
 * rtmp_frame_got 置位用于调用方判断“这一 tag 是否真的产出了帧”。
 */
static void ingest_rtmp_demux_on_frame(const zcs_frame *frame, void *user)
{
    zcs_live_ingest *ch = (zcs_live_ingest *)user;
    zcs_frame out;
    zn_buf *buf;
    uint8_t *dst;

    if (!ch || !frame || !frame->data || frame->size == 0 || !ch->source ||
        !ch->source->gop_queue) {
        return;
    }

    ch->rtmp_frame_got = 1;
    buf = zcs_gop_queue_alloc_write(ch->source->gop_queue, frame->size);
    if (!buf) {
        return;
    }
    dst = (uint8_t *)zn_buf_data(buf);
    memcpy(dst, frame->data, frame->size);
    zn_buf_set_len(buf, frame->size);

    zcs_frame_init(&out);
    out.data = dst;
    out.size = frame->size;
    out.codec = frame->codec;
    out.track = frame->track;
    out.keyframe = frame->keyframe;
    out.owned = 0;

    if (frame->track == ZCS_TRACK_VIDEO) {
        uint32_t ts = live_ingest_video_pts(ch, ch->rtmp_ingress.raw_tag_dts_ms);
        out.dts_ms = out.pts_ms = ts;
        if (frame->codec == ZCS_CODEC_H264) {
            zcs_live_ingest_h264_annexb_frame_flags(dst, frame->size, &out.config_frame,
                                                    &out.drop_able);
        }
        ch->source->has_video = 1;
    } else if (frame->track == ZCS_TRACK_AUDIO) {
        uint32_t ts = live_ingest_audio_pts(ch, ch->rtmp_ingress.raw_tag_dts_ms);
        const uint8_t *raw = dst;
        size_t raw_len = frame->size;

        out.dts_ms = out.pts_ms = ts;
        if (frame->codec == ZCS_CODEC_AAC &&
            zcs_aac_es_to_raw(dst, frame->size, &raw, &raw_len) == ZN_OK && raw != dst) {
            memmove(dst, raw, raw_len);
            out.size = raw_len;
            zn_buf_set_len(buf, raw_len);
        }
        ch->source->has_audio = 1;
    } else {
        zn_buf_unref(buf);
        return;
    }

    out.data = (uint8_t *)zn_buf_data(buf);
    (void)live_ingest_write_frame_buf(ch, buf, &out);
}

static void ingest_ensure_rtmp_demux(zcs_live_ingest *ch)
{
    zcs_demux_pipeline_opts cfg;

    if (!ch || ch->rtmp_demux) {
        return;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.container = ZCS_CONTAINER_FLV_TAG;
    cfg.on_frame = ingest_rtmp_demux_on_frame;
    cfg.user = ch;
    ch->rtmp_demux = zcs_demux_pipeline_create(&cfg);
}

zcs_demux_pipeline *zcs_live_ingest_rtmp_demux_create(zcs_live_ingest *ch)
{
    if (!ch) {
        return NULL;
    }
    ingest_ensure_rtmp_demux(ch);
    return ch->rtmp_demux;
}

void zcs_live_ingest_rtmp_demux_release(zcs_live_ingest *ch)
{
    if (ch) {
        ch->rtmp_demux = NULL;
    }
}

static zn_err_t ingest_rtmp_feed_tag(zcs_live_ingest *ch, uint8_t type_id, uint32_t tag_dts_ms,
                                      const void *body, size_t body_len, uint8_t msg_type)
{
    zn_err_t err;

    if (!ch || !body || body_len == 0) {
        return ZN_ERR_INVALID;
    }
    if (!ch->source || !ch->source->gop_queue) {
        return ZN_ERR_INVALID;
    }

    ingest_ensure_rtmp_demux(ch);
    if (!ch->rtmp_demux) {
        return ZN_ERR_NOMEM;
    }

    ch->rtmp_ingress.active = 1;
    ch->rtmp_ingress.raw_tag_dts_ms = tag_dts_ms;
    ch->rtmp_ingress.msg_type = msg_type;
    ch->rtmp_frame_got = 0;

    err = zcs_demux_pipeline_input_flv_tag(ch->rtmp_demux, 0, type_id, (const uint8_t *)body,
                                           body_len, tag_dts_ms);
    ch->rtmp_ingress.active = 0;
    return err;
}

zn_err_t zcs_live_ingest_input_rtmp_video(zcs_live_ingest *ch, uint32_t tag_dts_ms,
                                           const void *body, size_t body_len)
{
    zcs_flv_tag_packet_kind vpkt;

    if (!ch || !body || body_len < 2) {
        return ZN_ERR_INVALID;
    }
    const uint8_t *p = (const uint8_t *)body;

    zcs_codec_id vc = zcs_flv_tag_video_codec(p, body_len);
    /* 探测不出 codec（tag 头畸形/首帧前）时退回源已记录的 codec；若源也没有，
     * 只能判不支持。这是为了容忍部分推流端首个 raw 帧码率字段不规范的情况。 */
    if (vc != ZCS_CODEC_H264 && vc != ZCS_CODEC_H265 && vc != ZCS_CODEC_H266 &&
        vc != ZCS_CODEC_AV1 && vc != ZCS_CODEC_VP8 && vc != ZCS_CODEC_VP9) {
        if (ch->have_video_cfg && ch->source->video.codec != ZCS_CODEC_INVALID) {
            vc = ch->source->video.codec;
        } else {
            zcs_log_warn_throttle("ingress:unsupported_rtmp_video", ZCS_LOG_THROTTLE_WARN_MS,
                                  "[ingress] unsupported_rtmp_video len=%u first=0x%02x",
                                  (unsigned)body_len, p[0]);
            return ZN_ERR_NOT_IMPL;
        }
    }

    vpkt = zcs_flv_tag_video_packet_kind(p, body_len);
    if (vpkt == ZCS_FLV_TAG_PKT_INVALID) {
        zcs_log_warn_throttle("ingress:bad_rtmp_video_hdr", ZCS_LOG_THROTTLE_WARN_MS,
                              "[ingress] bad_rtmp_video_hdr len=%u first=0x%02x",
                              (unsigned)body_len, p[0]);
        return ZN_ERR_INVALID;
    }

    if (vpkt == ZCS_FLV_TAG_PKT_SEQ_HEADER) {
        zcs_codec_id ac = ZCS_CODEC_INVALID;
        if (ch->source->has_audio && ch->source->audio.codec != ZCS_CODEC_INVALID) {
            ac = ch->source->audio.codec;
        }
        /* 在真正接纳配置前做能力校验：把“即将建立的音视频组合”交给会话
         * 能力表判定，不支持的组合尽早拒绝，避免建好一半管线才发现播不了。 */
        if (zcs_session_capability_check(ZCS_PROTO_CAP_RTMP_PUBLISH, vc, ac, 1,
                                         ch->source->has_audio && ac != ZCS_CODEC_INVALID) !=
            ZN_OK) {
            zcs_session_capability_log_reject("rtmp-publish", ch->source,
                                              ZCS_PROTO_CAP_RTMP_PUBLISH);
            return ZN_ERR_NOT_IMPL;
        }
        ch->have_video_cfg = 1;
        ch->source->has_video = 1;
        ch->source->video.codec = vc;
        live_ingest_set_video_config(ch, body, body_len);
        if (vc == ZCS_CODEC_H264) {
            (void)zcs_video_track_from_avc(&ch->source->video, p, body_len);
        } else {
            ch->source->video.ready = 1;
        }
        zn_debug("[ingress] track_video codec=%s cfg_len=%u", zcs_codec_name(vc),
                  (unsigned)body_len);
        (void)ingest_rtmp_feed_tag(ch, 9, tag_dts_ms, p, body_len, ZCS_RTMP_MSG_VIDEO);
        return ZN_OK;
    }

    if (vpkt != ZCS_FLV_TAG_PKT_RAW) {
        return ZN_OK;
    }

    {
        zn_err_t err;

        err = ingest_rtmp_feed_tag(ch, 9, tag_dts_ms, p, body_len, ZCS_RTMP_MSG_VIDEO);
        /* rtmp_frame_got 表示 demux 确实产出了帧。若没产出但也没报错，视为
         * 正常（例如 demux 内部缓存阶段/首帧探测），返回 OK，避免调用方把
         * 这种常态误判为失败而中断推流。 */
        if (ch->rtmp_frame_got) {
            return ZN_OK;
        }
        if (err != ZN_OK) {
            zcs_log_warn_throttle("ingress:rtmp_demux_fail", ZCS_LOG_THROTTLE_WARN_MS,
                                  "[ingress] rtmp_demux_fail len=%u first=0x%02x pkt=%d",
                                  (unsigned)body_len, p[0], (int)vpkt);
            return err;
        }
        return ZN_OK;
    }
}

static int ingest_aac_seq_header_valid(const uint8_t *body, size_t body_len, int *hdr_len)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;
    int sr = 0;
    int ch_n = 0;

    if (hdr_len) {
        *hdr_len = 0;
    }
    if (!body || body_len < 4) {
        return 0;
    }
    if (zcs_flv_tag_aac_seq_header_asc(body, body_len, &asc, &asc_len) != ZN_OK) {
        return 0;
    }
    if (asc_len < 2 || !zcs_aac_parse_asc(asc, asc_len, &sr, &ch_n)) {
        return 0;
    }
    if (hdr_len) {
        *hdr_len = (int)(asc - body);
    }
    return 1;
}

zn_err_t zcs_live_ingest_input_rtmp_audio(zcs_live_ingest *ch, uint32_t tag_dts_ms,
                                           const void *body, size_t body_len)
{
    zcs_codec_id acodec;

    if (!ch || !body || body_len < 2) {
        return ZN_ERR_INVALID;
    }
    const uint8_t *p = (const uint8_t *)body;
    acodec = zcs_flv_tag_audio_codec(p, body_len);
    /* Opus 走 RTMP 的 seq header 形态当前不支持：能力表会拒绝，避免把这
     * 类流误接入后下游无法解码。G711 同理在下方拒绝。 */
    if (acodec == ZCS_CODEC_OPUS &&
        zcs_flv_tag_audio_packet_kind(p, body_len) == ZCS_FLV_TAG_PKT_SEQ_HEADER) {
        zcs_session_capability_log_reject("rtmp-publish", ch->source, ZCS_PROTO_CAP_RTMP_PUBLISH);
        return ZN_ERR_NOT_IMPL;
    }
    if (ingest_aac_seq_header_valid(p, body_len, NULL)) {
        /* 只处理首个 audio seq header（have_audio_cfg 之后忽略重复的），
         * 同时更新 timeline 的音频采样率——ADTS 帧不含采样率，后续把 raw AAC
         * 转 ADTS 需要这里定下的参数。最后把 ASC 也喂给 demux，保证 profile
         * 一致。 */
        if (!ch->have_audio_cfg) {
            zcs_codec_id vc = ZCS_CODEC_INVALID;
            if (ch->source->has_video && ch->source->video.codec != ZCS_CODEC_INVALID) {
                vc = ch->source->video.codec;
            }
            if (zcs_session_capability_check(ZCS_PROTO_CAP_RTMP_PUBLISH, vc, ZCS_CODEC_AAC,
                                             ch->source->has_video && vc != ZCS_CODEC_INVALID,
                                             1) != ZN_OK) {
                zcs_session_capability_log_reject("rtmp-publish", ch->source,
                                                  ZCS_PROTO_CAP_RTMP_PUBLISH);
                return ZN_ERR_NOT_IMPL;
            }
            live_ingest_set_audio_config(ch, body, body_len);
            ch->have_audio_cfg = 1;
            ch->source->has_audio = 1;
            (void)zcs_audio_track_from_rtmp(&ch->source->audio, p, body_len);
            zcs_media_timeline_set_audio(&ch->tl, ZCS_CODEC_AAC,
                                         ch->source->audio.sample_rate > 0
                                             ? (uint32_t)ch->source->audio.sample_rate
                                             : 44100);
            zn_debug("[ingress] track_audio codec=AAC cfg_len=%u", (unsigned)body_len);
            /* 让 flv_demuxer 也加载 ASC；否则后续 raw AAC 帧转 ADTS 时 profile=0。 */
            (void)ingest_rtmp_feed_tag(ch, 8, tag_dts_ms, p, body_len, ZCS_RTMP_MSG_AUDIO);
        }
        return ZN_OK;
    }
    if (acodec == ZCS_CODEC_G711A || acodec == ZCS_CODEC_G711U) {
        zcs_session_capability_log_reject("rtmp-publish", ch->source, ZCS_PROTO_CAP_RTMP_PUBLISH);
        return ZN_ERR_NOT_IMPL;
    }

    {
        zn_err_t err;

        err = ingest_rtmp_feed_tag(ch, 8, tag_dts_ms, p, body_len, ZCS_RTMP_MSG_AUDIO);
        if (ch->rtmp_frame_got) {
            return ZN_OK;
        }
        if (err != ZN_OK) {
            return err;
        }
        /* raw AAC tag demux pipeline 处理后未触发 on_frame 属正常情况
         * （如首帧 codec 探测期）；返回 OK 避免调用方误判为错误 */
        return ZN_OK;
    }
}
