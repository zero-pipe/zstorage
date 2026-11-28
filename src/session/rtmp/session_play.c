/**
 * @file session_play.c
 * @brief RTMP 直播播放路径：bootstrap、kick、flush 与出站管线。
 *
 * 一次 RTMP play 的时序：
 *   onplay（在 on_recv 持 play_mtx 时）→ 记录目标/鉴权/重置状态 → 置 boot_pending
 *   → schedule_play_kick（**异步**投递到 poller，避开持锁内联死锁）
 *   → play_kick：attach 读者（VOD lane 或 gop）→ bootstrap 发 sequence header
 *   → play_kick_finish：arm clock，必要时突发几轮 flush 把首屏推出去。
 * 之后每个 tick 由 play_flush 驱动出站管线 pump + flush_tcp。
 *
 * 并发：除 onplay 外，kick/flush 都在会话锁保护下运行（zcs_rtmp_session_lock）；
 * _nolock 后缀的函数要求调用方已持锁。kill/kick 通过 destroy_token 防止对已
 * 销毁会话的延迟回调操作。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/play/vod_play_port.h"
#include "session/rtmp/session_internal.h"
#include "session/rtmp/session_transport.h"
#include "zn/platform.h"
#include "zcs/play/dispatch.h"
#include "zcs/play/play_binding.h"
#include "zcs/core/codec_filter.h"
#include "zcs/egress/pacing/egress_live_policy.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/media/codec/aac/aac_over_rtmp.h"
#include "zcs/util/hex_decode.h"
#include "media/container/flv/flv_file_muxer.h"
#include "zcs/core/event.h"
#include "zcs/media/wire/amf.h"
#include "zcs/core/gop/queue.h"
#include "zcs/media/wire/rtmp.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/clock.h"
#include "zcs/egress/pacing/egress_pacing.h"
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/util/buf_pool.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include "zcs/util/hex_decode.h"
#endif

typedef struct zcs_rtmp_play_kick_ctx {
    zcs_rtmp_session *s;
    unsigned token;
} zcs_rtmp_play_kick_ctx;

/* 会话是否仍存活：tcp 句柄存在且其 user 指针仍回指本会话。
 * 会话可能已被销毁而延迟的回调还在队列里，用这一对判断避免对野会话操作。 */
int zcs_rtmp_session_alive(zcs_rtmp_session *s)
{
    return s && s->tcp && zn_tcp_session_user(s->tcp) == s;
}

static void rtmp_play_kick_async(void *user)
{
    zcs_rtmp_play_kick_ctx *ctx = (zcs_rtmp_play_kick_ctx *)user;
    zcs_rtmp_session *s;
    unsigned tok;

    if (!ctx) {
        return;
    }
    s = ctx->s;
    tok = ctx->token;
    free(ctx);
    if (!s || s->destroy_token != tok || s->destroy_scheduled) {
        return;
    }
    zcs_rtmp_session_lock(s);
    if (!s->destroy_scheduled) {
        zcs_rtmp_session_play_kick(s);
    }
    zcs_rtmp_session_unlock(s);
}

static uint8_t *zcs_rtmp_session_tag_buf(zcs_rtmp_session *s, size_t need, uint8_t *stack,
                                         size_t stack_cap)
{
    zn_poller *pol;

    if (need <= stack_cap) {
        return stack;
    }
    pol = s && s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
    if (!s || !zcs_buf_pool_slot_resize_poller(&s->pl.tag_buf, &s->pl.tag_cap, need, pol)) {
        return NULL;
    }
    return s->pl.tag_buf;
}

static void rtmp_send_video_cfg_tag(zcs_rtmp_session *s, const uint8_t *cfg, size_t clen)
{
    uint8_t body_stack[4096];
    const uint8_t *body = NULL;
    size_t body_len = 0;
    zcs_codec_id fallback;

    if (!s || !s->proto || !cfg || clen == 0) {
        return;
    }
    fallback = (s->source && s->source->video.codec != ZCS_CODEC_INVALID) ? s->source->video.codec
                                                                          : ZCS_CODEC_INVALID;
    if (zcs_flv_video_cfg_body(cfg, clen, fallback, body_stack, sizeof(body_stack), &body,
                               &body_len) != 1) {
        return;
    }
    zcs_rtmp_session_send_media(s, 9, body, body_len, 0);
}

static const uint8_t *rtmp_vod_video_config(const zcs_rtmp_session *s, size_t *clen)
{
    const uint8_t *cfg;

    if (clen) {
        *clen = 0;
    }
    if (!s || !s->source) {
        return NULL;
    }
    if (s->pl.vod_lane) {
        cfg = zcs_vod_play_lane_video_config(s->pl.vod_lane, clen);
        if (cfg && clen && *clen > 0) {
            return cfg;
        }
    }
    return zcs_media_source_video_config(s->source, clen);
}

static const uint8_t *rtmp_vod_audio_config(const zcs_rtmp_session *s, size_t *clen)
{
    const uint8_t *cfg;

    if (clen) {
        *clen = 0;
    }
    if (!s || !s->source) {
        return NULL;
    }
    if (s->pl.vod_lane) {
        cfg = zcs_vod_play_lane_audio_config(s->pl.vod_lane, clen);
        if (cfg && clen && *clen > 0) {
            return cfg;
        }
    }
    return zcs_media_source_audio_config(s->source, clen);
}

/*
 * 把 RTMP play 的 start（秒）换算成毫秒。
 * 歧义兜底：start 既可能是“秒”也可能是某些客户端误传的“毫秒”。若已知时长且
 * start 大于时长的 1.5 倍，就按毫秒原样返回（否则“秒→毫秒”会得到远超时长、
 * 甚至越界的 seek 点）；否则按秒×1000。duration_arg 在本源没有时长信息时作参考。
 */
static uint64_t rtmp_play_pos_to_ms(double start, const zcs_media_source *src, double duration_arg)
{
    double dur_sec = 0;

    if (start <= 0.0) {
        return 0;
    }
    if (src && zcs_media_source_is_vod(src)) {
        dur_sec = zcs_vod_source_duration_ms(src) / 1000.0;
    } else if (duration_arg > 0.0) {
        dur_sec = duration_arg;
    }
    if (dur_sec > 0.0 && start > dur_sec * 1.5) {
        return (uint64_t)(start + 0.5);
    }
    return (uint64_t)(start * 1000.0 + 0.5);
}

static double rtmp_vod_file_size_bytes(const zcs_media_source *src)
{
    char path[512];
    struct stat st;

    if (!src || !zcs_vod_resolve_file_path(src->app, src->stream, path, sizeof(path))) {
        return 0.0;
    }
    if (stat(path, &st) != 0) {
        return 0.0;
    }
    return (double)st.st_size;
}

static size_t amf_encode_key(uint8_t *out, size_t cap, const char *key)
{
    size_t klen = key ? strlen(key) : 0;
    if (cap < 2 + klen) {
        return 0;
    }
    /* AMF0 对象成员名：U16 长度 + UTF-8（无类型字节） */
    out[0] = (uint8_t)((klen >> 8) & 0xff);
    out[1] = (uint8_t)(klen & 0xff);
    if (klen) {
        memcpy(out + 2, key, klen);
    }
    return 2 + klen;
}

static size_t amf_encode_bool(uint8_t *out, size_t cap, int v)
{
    if (cap < 2) {
        return 0;
    }
    out[0] = ZCS_AMF_BOOLEAN;
    out[1] = v ? 1 : 0;
    return 2;
}

void zcs_rtmp_session_send_data_onstatus(zcs_rtmp_session *s, const char *code, const char *desc)
{
    uint8_t amf[256];
    size_t pos = 0;

    if (!s || !s->proto || !code) {
        return;
    }
    pos += zcs_amf_encode_string(amf + pos, sizeof(amf) - pos, "onStatus");
    amf[pos++] = ZCS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "level");
    pos += zcs_amf_encode_string(amf + pos, sizeof(amf) - pos, "status");
    pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "code");
    pos += zcs_amf_encode_string(amf + pos, sizeof(amf) - pos, code);
    if (desc && desc[0]) {
        pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "description");
        pos += zcs_amf_encode_string(amf + pos, sizeof(amf) - pos, desc);
    }
    pos += zcs_amf_encode_object_end(amf + pos, sizeof(amf) - pos);
    zcs_rtmp_session_send_media(s, 18, amf, pos, 0);
}

static void zcs_rtmp_session_send_data_start(zcs_rtmp_session *s)
{
    uint8_t amf[128];
    size_t pos = 0;

    if (!s || !s->proto) {
        return;
    }
    pos += zcs_amf_encode_string(amf + pos, sizeof(amf) - pos, "onStatus");
    amf[pos++] = ZCS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "level");
    pos += zcs_amf_encode_string(amf + pos, sizeof(amf) - pos, "status");
    pos += amf_encode_key(amf + pos, sizeof(amf) - pos, "code");
    pos += zcs_amf_encode_string(amf + pos, sizeof(amf) - pos, "NetStream.Data.Start");
    pos += zcs_amf_encode_object_end(amf + pos, sizeof(amf) - pos);
    zcs_rtmp_session_send_media(s, 18, amf, pos, 0);
}

static size_t rtmp_build_play_onmetadata(zcs_rtmp_session *s, uint8_t *amf, size_t cap)
{
    size_t pos = 0;
    double width = 0, height = 0, fps = 0;
    double video_codec = 7.0, audio_codec = 10.0;
    double audio_rate = 44100.0, stereo = 1.0;
    double duration_sec = 0.0;
    zcs_codec_id vc = ZCS_CODEC_INVALID;

    if (!s || !s->source || !amf || cap == 0) {
        return 0;
    }

    if (zcs_media_source_is_vod(s->source)) {
        duration_sec = zcs_vod_source_duration_ms(s->source) / 1000.0;
    }

    if (s->source->video.ready) {
        vc = s->source->video.codec;
    }
    if (vc == ZCS_CODEC_INVALID && s->source->gop_queue) {
        size_t vlen = 0;
        const uint8_t *vcfg = zcs_media_source_video_config(s->source, &vlen);
        if (vcfg && vlen) {
            vc = zcs_flv_video_config_codec(vcfg, vlen);
        }
    }
    video_codec = zcs_flv_metadata_videocodecid(vc != ZCS_CODEC_INVALID ? vc : ZCS_CODEC_H264);

    if (zcs_media_source_is_vod(s->source)) {
        double file_size = rtmp_vod_file_size_bytes(s->source);
        if (file_size > 0.0) {
            pos += amf_encode_key(amf + pos, cap - pos, "fileSize");
            pos += zcs_amf_encode_number(amf + pos, cap - pos, file_size);
        }
    }

    if (s->source->video.ready) {
        width = (double)s->source->video.width;
        height = (double)s->source->video.height;
        fps = (double)s->source->video.fps;
    }
    if (s->source->audio.ready) {
        audio_rate = (double)s->source->audio.sample_rate;
        stereo = s->source->audio.channels > 1 ? 1.0 : 0.0;
    }

    pos += zcs_amf_encode_string(amf + pos, cap - pos, "onMetaData");
    amf[pos++] = ZCS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, cap - pos, "duration");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, duration_sec);
    if (duration_sec > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "canSeekToEnd");
        pos += amf_encode_bool(amf + pos, cap - pos, 1);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "width");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, width);
    pos += amf_encode_key(amf + pos, cap - pos, "height");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, height);
    if (fps > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "framerate");
        pos += zcs_amf_encode_number(amf + pos, cap - pos, fps);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "videocodecid");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, video_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiocodecid");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, audio_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiosamplerate");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, audio_rate);
    pos += amf_encode_key(amf + pos, cap - pos, "stereo");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, stereo);
    pos += zcs_amf_encode_object_end(amf + pos, cap - pos);
    return pos;
}

static void zcs_rtmp_session_send_play_onmetadata(zcs_rtmp_session *s)
{
    uint8_t stack[4096];
    size_t meta_len;

    if (!s || !s->proto || !s->source) {
        return;
    }

    /* 播放侧 onMetaData：ffmpeg 对 videocodecid=12 / Enhanced hvc1 tag 更友好，便于 ffplay 走 legacy 路径。 */
    meta_len = rtmp_build_play_onmetadata(s, stack, sizeof(stack));
    if (meta_len == 0) {
        return;
    }
    zcs_rtmp_session_send_media(s, 18, stack, meta_len, 0);
}

/* 出站管线 tag 回调：把 FLV tag 按类型映射到 RTMP message type（8=audio,9=video）
 * 交给传输层。首次分别打印一条 first_audio/first_video 便于排查“只有视频没音频”
 * 之类问题。 */
static void rtmp_egress_on_tag(uint8_t msg_type, uint32_t tag_dts_ms, const uint8_t *body,
                               size_t len, void *user)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)user;

    if (!s || !s->proto || !body || len == 0) {
        return;
    }
    if (msg_type == ZCS_RTMP_MSG_AUDIO) {
        zcs_rtmp_session_send_media(s, 8, body, len, tag_dts_ms);
        if (!s->pl.logged_audio) {
            s->pl.logged_audio = 1;
            zn_debug("[rtmp] first_audio session=%u ts=%u len=%u", s->session_no,
                      (unsigned)tag_dts_ms, (unsigned)len);
        }
    } else if (msg_type == ZCS_RTMP_MSG_VIDEO) {
        zcs_rtmp_session_send_media(s, 9, body, len, tag_dts_ms);
        if (!s->pl.logged_video) {
            s->pl.logged_video = 1;
            zn_debug("[rtmp] first_video session=%u ts=%u len=%u", s->session_no,
                      (unsigned)tag_dts_ms, (unsigned)len);
        }
    }
}

static void rtmp_play_resend_stream_config(zcs_rtmp_session *s)
{
    size_t clen;
    uint8_t cfg_stack[4096];
    uint8_t *cfg_buf;

    if (!s || !s->proto || !s->source) {
        return;
    }
    if (!s->source->gop_queue) {
        return;
    }
    clen = 0;
    {
        size_t cfg_cap = 0;

        (void)zcs_media_source_video_config(s->source, &cfg_cap);
        cfg_buf =
            zcs_rtmp_session_tag_buf(s, cfg_cap > 0 ? cfg_cap : 1, cfg_stack, sizeof(cfg_stack));
        clen =
            cfg_buf ? zcs_gop_queue_copy_video_config(s->source->gop_queue, cfg_buf, cfg_cap) : 0;
        if (clen) {
            rtmp_send_video_cfg_tag(s, cfg_buf, clen);
        }
    }
    {
        uint8_t acfg_stack[4096];
        uint8_t *acfg_buf;
        size_t acfg_cap = 0;

        (void)zcs_media_source_audio_config(s->source, &acfg_cap);
        acfg_buf = zcs_rtmp_session_tag_buf(s, acfg_cap > 0 ? acfg_cap : 1, acfg_stack,
                                            sizeof(acfg_stack));
        clen = acfg_buf ? zcs_gop_queue_copy_audio_config(s->source->gop_queue, acfg_buf, acfg_cap)
                        : 0;
        if (clen) {
            zcs_rtmp_session_send_media(s, 8, acfg_buf, clen, 0);
        }
    }
}

/* 出站管线要求“跳到直播边缘”时的回调：解除视频 armed 并解锁 epoch，
 * 重新发送 sequence header——snap 后解码器需要新的 SPS/PPS 才能从新的关键帧起播。 */
static void rtmp_play_on_snap(void *user)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)user;

    if (!s) {
        return;
    }
    s->pl.video_armed = 0;
    zcs_egress_clock_unlock(&s->pl.clk);
    rtmp_play_resend_stream_config(s);
}

/* 慢消费者踢出：出站 outbox 积压到阈值说明该观众网速跟不上，直接断开比无限缓冲
 * 更合理——否则延迟只增不减，且占用出站资源。 */
static void rtmp_play_on_slow_consumer(void *user)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)user;

    if (!s || s->destroy_scheduled) {
        return;
    }
    zn_warn("[rtmp] reader_kick session=%u", s->session_no);
    zcs_rtmp_session_play_teardown(s);
    s->state = ZCS_RTMP_SESSION_STATE_IDLE;
    s->source = NULL;
    if (s->tcp) {
        zn_tcp_session_close(s->tcp);
    }
}

void zcs_rtmp_session_egress_create(zcs_rtmp_session *s)
{
    zcs_egress_pipeline_opts ecfg;
    zcs_egress_flv_bind fbind;

    if (!s || !s->source) {
        return;
    }
    zcs_egress_pipeline_destroy(s->pl.egress_pipe);
    s->pl.egress_pipe = NULL;
    if (!s->pl.src.readers.gop && !s->pl.src.readers.vod && !s->pl.vod_reader) {
        return;
    }
    memset(&ecfg, 0, sizeof(ecfg));
    memset(&fbind, 0, sizeof(fbind));
    fbind.source = s->source;
    fbind.play_clk = &s->pl.clk;
    fbind.timeline = &s->pl.timeline;
    fbind.on_tag = rtmp_egress_on_tag;
    fbind.user = s;
    fbind.video_armed = &s->pl.video_armed;
    ecfg.wire = ZCS_WIRE_FORMAT_RTMP;
    ecfg.reader = &s->pl.src;
    ecfg.flv = &fbind;
    s->pl.egress_pipe = zcs_egress_pipeline_create(&ecfg);
    if (s->pl.egress_pipe && s->tcp) {
        zcs_egress_pipeline_bind_poller(s->pl.egress_pipe, zn_tcp_session_poller(s->tcp));
    }
}

void zcs_rtmp_session_egress_close(zcs_rtmp_session *s)
{
    zcs_play_binding bind;

    if (!s) {
        return;
    }
    zcs_egress_pipeline_destroy(s->pl.egress_pipe);
    s->pl.egress_pipe = NULL;
    memset(&bind, 0, sizeof(bind));
    bind.source = &s->source;
    bind.play = &s->pl.src;
    bind.gop_reader = &s->pl.gop_reader;
    bind.vod_reader = &s->pl.vod_reader;
    bind.vod_lane = &s->pl.vod_lane;
    bind.reader_attached = &s->pl.reader_attached;
    bind.play_start_ms = &s->pl.start_ms;
    bind.player = "rtmp";
    zcs_play_binding_close_readers(&bind);
}

/* 播放 bootstrap：发送 onMetaData/onStatus + 音视频 sequence header。
 * 顺序约定：**先 video seq header 再 audio seq header**——ffmpeg 的 RTMP 输入
 * 对“先看到视频序列头”更稳，反序偶发解码器初始化问题。VOD 不发 Data.Start/
 * onMetaData 的直播元信息（见下方注释），避免 ffmpeg 报 Invalid data。 */
void zcs_rtmp_session_play_bootstrap(zcs_rtmp_session *s)
{
    size_t clen;

    if (!s || !s->proto || !s->source || s->pl.boot_sent) {
        return;
    }

    /*
     * VOD 不在此发 onMetaData / Data.Start / PublishNotify，否则 ffmpeg RTMP 可能报 Invalid data。
     * 直播 bootstrap：plain onMetaData + hvc1。
     */
    if (!zcs_media_source_is_vod(s->source)) {
        zcs_rtmp_session_send_data_start(s);
        zcs_rtmp_session_send_data_onstatus(s, "NetStream.Play.PublishNotify", "Now published.");
        zcs_rtmp_session_send_play_onmetadata(s);
    }

    /* 兼容 ffmpeg：先发 video seq header，再发 audio seq header */
    clen = 0;
    if (s->source->gop_queue) {
        size_t cfg_cap = 0;
        uint8_t cfg_stack[4096];
        uint8_t *cfg_buf;

        (void)zcs_media_source_video_config(s->source, &cfg_cap);
        cfg_buf =
            zcs_rtmp_session_tag_buf(s, cfg_cap > 0 ? cfg_cap : 1, cfg_stack, sizeof(cfg_stack));
        clen =
            cfg_buf ? zcs_gop_queue_copy_video_config(s->source->gop_queue, cfg_buf, cfg_cap) : 0;
        if (clen) {
            rtmp_send_video_cfg_tag(s, cfg_buf, clen);
        }
    } else if (zcs_media_source_is_vod(s->source)) {
        const uint8_t *vcfg = rtmp_vod_video_config(s, &clen);
        uint8_t cfg_stack[4096];
        uint8_t *cfg_buf;

        if (vcfg && clen) {
            cfg_buf = zcs_rtmp_session_tag_buf(s, clen, cfg_stack, sizeof(cfg_stack));
            if (cfg_buf) {
                memcpy(cfg_buf, vcfg, clen);
                rtmp_send_video_cfg_tag(s, cfg_buf, clen);
            }
        }
    }

    clen = 0;
    if (s->source->gop_queue) {
        uint8_t acfg_stack[4096];
        uint8_t *acfg_buf;
        size_t acfg_cap = 0;

        (void)zcs_media_source_audio_config(s->source, &acfg_cap);
        acfg_buf = zcs_rtmp_session_tag_buf(s, acfg_cap > 0 ? acfg_cap : 1, acfg_stack,
                                            sizeof(acfg_stack));
        clen = acfg_buf ? zcs_gop_queue_copy_audio_config(s->source->gop_queue, acfg_buf, acfg_cap)
                        : 0;
        if (clen) {
            zcs_rtmp_session_send_media(s, 8, acfg_buf, clen, 0);
        }
    } else if (zcs_media_source_is_vod(s->source)) {
        const uint8_t *acfg = rtmp_vod_audio_config(s, &clen);
        if (acfg && clen) {
            zcs_rtmp_session_send_media(s, 8, acfg, clen, 0);
        }
    } else if (s->source->has_audio) {
        uint8_t asc[16];
        size_t asc_len = 0;
        if (s->source->audio.ready && s->source->audio.asc_hex[0]) {
            if (zcs_hex_decode(s->source->audio.asc_hex, asc, sizeof(asc), &asc_len) != ZN_OK) {
                asc_len = 0;
            }
        }
        if (asc_len == 0) {
            char hex[16];
            int rate = s->source->audio.ready ? s->source->audio.sample_rate : 44100;
            int ch = s->source->audio.ready ? s->source->audio.channels : 2;
            if (zcs_aac_build_config_hex(rate, ch, hex, sizeof(hex)) &&
                zcs_hex_decode(hex, asc, sizeof(asc), &asc_len) == ZN_OK) {
                zn_debug("[rtmp] synthesize_aac_config session=%u hex=%s", s->session_no, hex);
            }
        }
        if (asc_len) {
            uint8_t buf[32];
            size_t tag_len = 0;
            if (zcs_rtmp_aac_seq_header(asc, asc_len, buf, sizeof(buf), &tag_len) == ZN_OK) {
                zcs_rtmp_session_send_media(s, 8, buf, tag_len, 0);
            }
        }
    }

    s->pl.boot_sent = 1;
}

void zcs_rtmp_session_play_fail(zcs_rtmp_session *s)
{
    if (!s) {
        return;
    }
    zcs_rtmp_session_play_teardown(s);
    s->state = ZCS_RTMP_SESSION_STATE_IDLE;
    s->source = NULL;
    if (s->tcp) {
        zn_tcp_session_close(s->tcp);
    }
}

void zcs_rtmp_session_play_kick_finish(zcs_rtmp_session *s)
{
    if (!s) {
        return;
    }

    if (!s->pl.vod_reader && s->pl.gop_reader) {
        zcs_egress_clock_arm(&s->pl.clk);
        zcs_egress_live_seek_rtsp_attach(s->pl.gop_reader);
    }

    zcs_rtmp_session_play_bootstrap(s);
    if (s->source && zcs_media_source_is_vod(s->source)) {
        s->pl.video_armed = 1;
    }

    if (s->source && !s->pl.reader_attached) {
        zcs_media_source_reader_add(s->source);
        s->pl.start_ms = zn_monotonic_ms();
        zcs_media_event_play(s->source, "rtmp");
        s->pl.reader_attached = 1;
    }

    zn_info("[rtmp] play_start session=%u app=%s stream=%s video=%d audio=%d es=%d vod_lag=%zu",
             s->session_no, s->app, s->stream, s->source ? s->source->has_video : 0,
             s->source ? s->source->has_audio : 0, s->pl.gop_reader ? 1 : 0,
             s->pl.vod_reader ? zcs_vod_buffer_reader_lag(s->pl.vod_reader) : 0);

    s->pl.boot_pending = 0;
    zcs_rtmp_session_play_flush_nolock(s);
    /* 首屏突发：直播源在 attach 后可能还没有可发的关键帧（pacing 未到期或 lag 计算
     * 尚未成立）。这里最多连冲 12 轮，直到 video_armed 或轮数耗尽——目的是尽快把
     * 首个 IDR 推给播放器，缩短首屏时间；12 是经验上界，再多收益极低且空转。
     */
    if (s->pl.gop_reader && !s->pl.video_armed) {
        int burst;

        for (burst = 0; burst < 12 && !s->pl.video_armed; ++burst) {
            zcs_rtmp_session_play_flush_nolock(s);
        }
    }
}

void zcs_rtmp_session_schedule_play_kick(zcs_rtmp_session *s)
{
    zcs_rtmp_play_kick_ctx *ctx;
    zn_poller *pol;

    if (!s || !s->pl.boot_pending) {
        return;
    }
    pol = s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
    /*
     * onplay 在 zcs_rtmp_session_lock（on_recv）下运行。may_sync=1 会在 poller 线程内联
     * play_kick，与非递归 play_mtx 死锁。
     */
    if (!pol) {
        zcs_rtmp_session_play_kick(s);
        return;
    }
    ctx = (zcs_rtmp_play_kick_ctx *)malloc(sizeof(*ctx));
    if (!ctx) {
        zcs_rtmp_session_play_kick(s);
        return;
    }
    ctx->s = s;
    ctx->token = s->destroy_token;
    if (zn_poller_async(pol, rtmp_play_kick_async, ctx, 0) != ZN_OK) {
        free(ctx);
        zcs_rtmp_session_play_kick(s);
    }
}

/*
 * play 命令的收尾（flush 首包、VOD attach 或直播读者 attach，再 bootstrap）。
 * VOD 走两条路：优先异步 lane（打开文件耗时，不能在 on_recv 线程同步做），
 * 失败再退回同步 attach；attach 失败即 play_fail。直播则补挂 gop 读者后 finish。
 */
void zcs_rtmp_session_play_kick(zcs_rtmp_session *s)
{
    if (!s || s->destroy_scheduled || !s->pl.boot_pending) {
        return;
    }

    /* Play.Start 已由 play 命令 dispatch 发出。 */
    zcs_rtmp_session_flush_tcp(s);

    if (s->source && zcs_media_source_is_vod(s->source)) {
        uint64_t start_ms = s->pl.vod_start_ms;

        s->pl.vod_start_ms = 0;
        if (!s->pl.vod_lane && zcs_rtmp_session_vod_lane_try_async(s, start_ms)) {
            return;
        }
        if (!zcs_rtmp_session_vod_attach(s, start_ms)) {
            zn_warn("[rtmp] vod_attach_failed session=%u app=%s stream=%s", s->session_no, s->app,
                     s->stream);
            zcs_rtmp_session_play_fail(s);
            return;
        }
        zcs_rtmp_session_play_kick_vod_prime(s, start_ms);
        zcs_rtmp_session_play_kick_finish(s);
        return;
    }

    if (!s->pl.gop_reader) {
        zcs_rtmp_session_play_readers_attach(s);
    }
    zcs_rtmp_session_play_kick_finish(s);
}

/*
 * RTMP play 命令入口（on_recv 线程，持 play_mtx）。
 * 顺序：抽查询串（token）→ 解析源 → 计算 seek 点 → 已在播则走重定位（VLC 拖拽）
 * → 鉴权 → 能力校验 → 重置播放状态、置 boot_pending。真正 attach/发送延后到
 * schedule_play_kick 的异步回调，避免在持锁的 on_recv 里内联 kick 造成死锁。
 * 返回 ZCS_RTMP_ASYNC_START 表示“已受理、稍后异步启动”。
 */
int rtmp_srv_onplay(void *param, const char *app_name, const char *stream_name, double start,
                    double duration, uint8_t reset)
{
    zcs_rtmp_session *s = (zcs_rtmp_session *)param;
    zcs_auth_request auth;
    zcs_auth_decision dec;
    char query[ZCS_AUTH_PARAMS_MAX];
    char ip[64];
    unsigned port = 0;
    uint64_t start_ms;

    if (!s) {
        return -1;
    }

    /* play 路径上的查询串（token 等）先摘出来，再交给按名字查流。 */
    zcs_auth_split_query(stream_name, NULL, 0, query, sizeof(query));
    if (query[0]) {
        zcs_auth_params_copy(query, s->params, sizeof(s->params));
    }
    zcs_auth_split_query(app_name, NULL, 0, query, sizeof(query));
    if (query[0]) {
        zcs_auth_params_copy(query, s->params, sizeof(s->params));
    }

    s->source = zcs_rtmp_session_find_play_source(s->vhost, app_name, stream_name, s->app, sizeof(s->app),
                                                  s->stream, sizeof(s->stream));
    start_ms = rtmp_play_pos_to_ms(start, s->source, duration);
    zn_info("[rtmp] play_request session=%u app=%s stream=%s start=%.3f duration=%.3f reset=%u "
             "seek_ms=%llu",
             s->session_no, s->app, s->stream, start, duration, (unsigned)reset,
             (unsigned long long)start_ms);
    if (!s->source ||
        (!zcs_media_source_use_gop_queue_play(s->source) && !zcs_media_source_is_vod(s->source))) {
        zn_warn("[rtmp] play_not_found session=%u app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        return -1;
    }

    /* VLC/ffplay 拖拽：会话已在播放时第二次 play(start>0) 或 play(reset) */
    if (s->state == ZCS_RTMP_SESSION_STATE_PLAYING && !s->pl.boot_pending && s->pl.vod_lane &&
        (start_ms > 0 || reset)) {
        if (start_ms == 0 && reset) {
            start_ms = 0;
        }
        return zcs_rtmp_session_vod_replay_seek(s, start_ms);
    }

    /* 播放鉴权：上游签发的 token，本地同步验签。 */
    zcs_auth_fill_peer(s->tcp, ip, sizeof(ip), &port);
    memset(&auth, 0, sizeof(auth));
    auth.action = ZCS_AUTH_ACTION_PLAY;
    auth.schema = ZCS_SCHEMA_RTMP;
    auth.vhost = s->vhost;
    auth.app = s->app;
    auth.stream = s->stream;
    auth.params = s->params[0] ? s->params : NULL;
    auth.ip = ip[0] ? ip : NULL;
    auth.port = port;
    if (!zcs_auth_check(&auth, &dec)) {
        zn_warn("[rtmp] play_denied session=%u app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        s->source = NULL;
        return -1;
    }

    if (!zcs_media_source_is_vod(s->source) &&
        zcs_session_capability_check_source(ZCS_PROTO_CAP_RTMP_PLAY, s->source) != ZN_OK) {
        zcs_session_capability_log_reject("rtmp", s->source, ZCS_PROTO_CAP_RTMP_PLAY);
        return -1;
    }

    s->pl.vod_start_ms = start_ms;

    s->pl.boot_sent = 0;
    s->pl.video_armed = 0;
    s->pl.live_catchup = 0;
    s->pl.lag_resync_ms = 0;
    s->pl.vod_pause_pos_valid = 0;
    s->pl.logged_audio = 0;
    s->pl.logged_video = 0;
    zcs_egress_clock_init(&s->pl.clk);
    zcs_egress_clock_set_scale(&s->pl.clk, 1.0);
    zcs_mux_av_timeline_reset(&s->pl.timeline);
    s->pl.boot_pending = 1;
    s->state = ZCS_RTMP_SESSION_STATE_PLAYING;

    /* 延后到 zcs_rtmp_transport_input 返回后再 async kick，避免与配置更新竞态导致 HLS 等路径 UAF */
    zcs_rtmp_session_schedule_play_kick(s);
    return ZCS_RTMP_ASYNC_START;
}

/*
 * 单次 flush 泵（调用方须持会话锁）。按数据来源分两路：
 *   - VOD：先按需补泵 lane（demux_fill 32 帧）保证有数据，再 pump_flv_vod；
 *     暂停时直接返回（不推进时间线）。
 *   - 直播：按 epoch/catchup/lag 算本次预算，pump_flv_live，并把 snap/慢消费者
 *     回调挂到 live_state 上。
 * 最后统一 flush_tcp 把 outbox 里的包真正写出去。
 */
void zcs_rtmp_session_play_flush_nolock(zcs_rtmp_session *s)
{
    if (!s || s->destroy_scheduled || s->state != ZCS_RTMP_SESSION_STATE_PLAYING ||
        !s->proto || !s->tcp) {
        return;
    }

    if (s->pl.egress_pipe) {
        if (s->pl.vod_reader) {
            zcs_flv_vod_egress_bind vcfg;

            if (zcs_egress_clock_is_paused(&s->pl.clk)) {
                return;
            }
            if (s->pl.vod_lane) {
                zcs_vod_play_lane_demux_fill(s->pl.vod_lane, 32);
            }
            memset(&vcfg, 0, sizeof(vcfg));
            vcfg.vod_rd = s->pl.vod_reader;
            vcfg.play_clk = &s->pl.clk;
            vcfg.catchup_left = &s->pl.vod_catchup;
            vcfg.es_buf = &s->pl.es_buf;
            vcfg.es_cap = &s->pl.es_cap;
            vcfg.pace_when_locked = 1;
            (void)zcs_egress_pipeline_pump_flv_vod(s->pl.egress_pipe, &vcfg, 0, s->session_no);
        } else if (s->pl.gop_reader) {
            zcs_egress_live_state live_st;
            size_t lag = zcs_gop_reader_lag(s->pl.gop_reader);
            int epoch_on = zcs_egress_clock_epoch_locked(&s->pl.clk);
            int budget = zcs_egress_live_pump_budget(epoch_on, s->pl.live_catchup, lag,
                                                     (int)ZCS_EGRESS_FLV_BUDGET_LIVE);

            memset(&live_st, 0, sizeof(live_st));
            live_st.live_catchup_done = &s->pl.live_catchup;
            live_st.live_resync_at_ms = &s->pl.lag_resync_ms;
            live_st.play_clk = &s->pl.clk;
            live_st.on_snap = rtmp_play_on_snap;
            live_st.snap_user = s;
            live_st.on_slow_consumer = rtmp_play_on_slow_consumer;
            live_st.slow_consumer_user = s;
            (void)zcs_egress_pipeline_pump_flv_live(s->pl.egress_pipe, budget, 500, s->session_no,
                                                    &live_st);
        }
        zcs_rtmp_session_flush_tcp(s);
    }
}

void zcs_rtmp_session_play_flush(zcs_rtmp_session *s)
{
    zcs_rtmp_session_lock(s);
    zcs_rtmp_session_play_flush_nolock(s);
    zcs_rtmp_session_unlock(s);
}
