/**
 * @file muxer.c
 * @brief 直播 HTTP-FLV 复用器（绑定媒体源与 poller）。
 */
#include "zms/play/http_flv/live_muxer.h"
#include "zms/core/codec_filter.h"
#include "media/container/flv/flv_file_muxer.h"
#include "zms/egress/source/egress_source.h"
#include "zms/media/clock.h"
#include "zms/media/media_limits.h"
#include "zms/core/stream/hub.h"
#include "zms/core/stream/stats.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/container/flv/flv_wire.h"
#include "zms/egress/pipeline/egress_pipeline.h"
#include "zms/play/dispatch.h"
#include "zms/util/buf_pool.h"
#include "zms/media/wire/rtmp.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

struct zms_flv_live_muxer {
    zms_media_source *source;
    zms_egress_source play;
    int header_have_audio;
    int header_has_video;
    int sent_metadata;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    int logged_video;
    int logged_audio;
    zms_mux_av_timeline mux;
    zms_flv_mux_pending pending;
    zms_egress_pipeline *egress_pipe;
    zn_poller *io_poller;
    uint8_t *tag_buf;
    size_t tag_cap;
    /* 因缓冲不足而暂存的 live tag（body 存于 tag_buf） */
    int has_held;
    uint8_t held_type;
    uint32_t held_ts;
    size_t held_len;
};

static void live_muxer_count_egress(zms_flv_live_muxer *m, size_t nbytes)
{
    if (m && m->source && nbytes > 0) {
        zms_media_stats_on_egress(m->source, nbytes);
    }
}

static zn_err_t live_attach_play(zms_flv_live_muxer *m)
{
    zms_session_play_opts pcfg;

    if (!m || !m->source) {
        return ZN_ERR_INVALID;
    }
    zms_session_dispatch_register_all();
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZMS_SESSION_HTTP_FLV;
    return zms_session_attach_play(ZMS_SESSION_HTTP_FLV, &m->play, m->source, &pcfg);
}

static void live_detach_play(zms_flv_live_muxer *m)
{
    if (!m) {
        return;
    }
    if (m->play.readers.gop) {
        zms_session_dispatch_register_all();
        zms_session_detach_play(ZMS_SESSION_HTTP_FLV, &m->play);
    }
}

static void live_egress_create(zms_flv_live_muxer *m)
{
    zms_egress_pipeline_opts ecfg;
    zms_egress_flv_bind fbind;

    if (!m || !m->play.readers.gop) {
        return;
    }
    zms_egress_pipeline_destroy(m->egress_pipe);
    m->egress_pipe = NULL;
    memset(&ecfg, 0, sizeof(ecfg));
    memset(&fbind, 0, sizeof(fbind));
    fbind.source = m->source;
    fbind.video_armed = &m->video_armed;
    fbind.timeline = &m->mux;
    ecfg.wire = ZMS_WIRE_FORMAT_HTTP_FLV;
    ecfg.reader = &m->play;
    ecfg.flv = &fbind;
    m->egress_pipe = zms_egress_pipeline_create(&ecfg);
    if (m->egress_pipe) {
        zms_egress_pipeline_bind_poller(m->egress_pipe, m->io_poller);
    }
}

static uint32_t live_out_tag_dts_ms(zms_flv_live_muxer *m, zms_track_type track,
                                    uint32_t tag_dts_ms)
{
    return zms_mux_av_timeline_pts(&m->mux, track, tag_dts_ms);
}

zms_flv_live_muxer *zms_flv_live_muxer_create(zms_media_source *src)
{
    zms_flv_live_muxer *m;

    if (!src || !zms_media_source_use_gop_queue_play(src)) {
        return NULL;
    }
    if (zms_session_capability_check_source(ZMS_PROTO_CAP_HTTP_FLV_PLAY, src) != ZN_OK) {
        return NULL;
    }
    m = (zms_flv_live_muxer *)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->source = src;
    if (live_attach_play(m) != ZN_OK) {
        free(m);
        return NULL;
    }
    live_egress_create(m);
    return m;
}

void zms_flv_live_muxer_bind_source(zms_flv_live_muxer *m, zms_media_source *src)
{
    if (m) {
        m->source = src;
    }
}

void zms_flv_live_muxer_bind_poller(zms_flv_live_muxer *m, zn_poller *pol)
{
    if (!m) {
        return;
    }
    m->io_poller = pol;
    if (m->egress_pipe) {
        zms_egress_pipeline_bind_poller(m->egress_pipe, pol);
    }
}

void zms_flv_live_muxer_destroy(zms_flv_live_muxer *m)
{
    if (!m) {
        return;
    }
    live_detach_play(m);
    zms_flv_mux_pending_clear(&m->pending);
    if (m->io_poller) {
        zms_buf_pool_slot_clear_poller(&m->tag_buf, &m->tag_cap, m->io_poller);
    } else {
        zms_buf_pool_slot_clear(&m->tag_buf, &m->tag_cap);
    }
    zms_egress_pipeline_destroy(m->egress_pipe);
    m->egress_pipe = NULL;
    free(m);
}

zn_err_t zms_flv_live_muxer_start(zms_flv_live_muxer *m, int has_audio, int has_video,
                                   uint8_t *out, size_t cap, size_t *out_len)
{
    size_t vlen = 0, alen = 0;

    if (!m || !out) {
        return ZN_ERR_INVALID;
    }
    if (m->source) {
        (void)zms_media_source_video_config(m->source, &vlen);
        (void)zms_media_source_audio_config(m->source, &alen);
    }
    m->header_have_audio = has_audio || alen > 0;
    m->header_has_video = has_video || vlen > 0;
    zms_mux_av_timeline_reset(&m->mux);
    if (m->source && m->source->audio.sample_rate > 0) {
        zms_codec_id ac = ZMS_CODEC_AAC;
        size_t clen = 0;
        const uint8_t *cfg = zms_media_source_audio_config(m->source, &clen);
        if (cfg && clen) {
            ac = zms_flv_tag_audio_codec(cfg, clen);
        }
        m->pending.audio_step_ms =
            zms_codec_frame_duration_ms(ac, (uint32_t)m->source->audio.sample_rate);
    }
    if (m->pending.audio_step_ms == 0) {
        m->pending.audio_step_ms = ZMS_MEDIA_TS_MIN_STEP_MS;
    }
    return zms_flv_write_header(out, cap, out_len, m->header_have_audio, m->header_has_video);
}

static int live_write_held(zms_flv_live_muxer *m, uint8_t *out, size_t cap, size_t *out_len)
{
    if (zms_flv_write_tag(out, cap, out_len, m->held_type, m->held_ts, m->tag_buf, m->held_len) !=
        ZN_OK) {
        if (out_len) {
            *out_len = m->held_len + ZMS_FLV_TAG_OVERHEAD;
        }
        return ZMS_SINK_NEED_BIGGER;
    }
    m->has_held = 0;
    live_muxer_count_egress(m, out_len ? *out_len : 0);
    return 1;
}

/* tag 已从 egress 管线取出，不能丢。复制到 tag_buf，并让调用方用更大缓冲再来一次。 */
static int live_hold_tag(zms_flv_live_muxer *m, const zms_egress_flv_tag *tag, size_t cap,
                         size_t *out_len)
{
    if (tag->len == 0) {
        return -1;
    }
    if (m->io_poller) {
        zms_buf_pool_slot_resize_poller(&m->tag_buf, &m->tag_cap, tag->len, m->io_poller);
    } else {
        zms_buf_pool_slot_resize(&m->tag_buf, &m->tag_cap, tag->len);
    }
    if (!m->tag_buf) {
        zn_warn("HTTP-FLV: hold tag alloc failed len=%u", (unsigned)tag->len);
        return -1;
    }
    memcpy(m->tag_buf, tag->body, tag->len);
    m->has_held = 1;
    m->held_type = tag->msg_type;
    m->held_ts = tag->tag_dts_ms;
    m->held_len = tag->len;
    if (out_len) {
        *out_len = tag->len + ZMS_FLV_TAG_OVERHEAD;
    }
    zn_debug("HTTP-FLV: tag needs %u bytes (cap=%u), retry with larger buffer",
             (unsigned)(tag->len + ZMS_FLV_TAG_OVERHEAD), (unsigned)cap);
    return ZMS_SINK_NEED_BIGGER;
}

int zms_flv_live_muxer_next(zms_flv_live_muxer *m, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!m || !out) {
        return -1;
    }

    if (!m->sent_metadata) {
        m->sent_metadata = 1;
    }

    if (!m->sent_video_cfg) {
        size_t clen = 0;
        const uint8_t *cfg = m->source ? zms_media_source_video_config(m->source, &clen) : NULL;
        if (cfg && clen) {
            zms_egress_flv_tag t = { ZMS_RTMP_MSG_VIDEO, 0, cfg, clen };
            int w;

            if (zms_flv_write_tag(out, cap, out_len, t.msg_type, t.tag_dts_ms, t.body, t.len) ==
                ZN_OK) {
                w = 1;
            } else {
                w = live_hold_tag(m, &t, cap, out_len);
            }
            if (w == ZMS_SINK_NEED_BIGGER) {
                return w;
            }
            if (w < 0) {
                return -1;
            }
            m->sent_video_cfg = 1;
            live_muxer_count_egress(m, out_len ? *out_len : 0);
            return 1;
        }
        m->sent_video_cfg = 1;
    }

    if (!m->sent_audio_cfg) {
        size_t clen = 0;
        const uint8_t *cfg = m->source ? zms_media_source_audio_config(m->source, &clen) : NULL;
        if (cfg && clen) {
            if (zms_flv_write_tag(out, cap, out_len, 8, 0, cfg, clen) != ZN_OK) {
                zms_egress_flv_tag t = { 8, 0, cfg, clen };

                return live_hold_tag(m, &t, cap, out_len);
            }
            m->sent_audio_cfg = 1;
            live_muxer_count_egress(m, out_len ? *out_len : 0);
            return 1;
        }
        m->sent_audio_cfg = 1;
    }

    if (m->pending.pending_emit < m->pending.pending_cnt) {
        int i = m->pending.pending_emit++;
        uint32_t out_ts = live_out_tag_dts_ms(m, ZMS_TRACK_AUDIO, m->pending.pending_tag_dts_ms[i]);
        if (!m->pending.pending_body[i] ||
            zms_flv_write_tag(out, cap, out_len, m->pending.pending_type[i], out_ts,
                              m->pending.pending_body[i], m->pending.pending_len[i]) != ZN_OK) {
            return -1;
        }
        live_muxer_count_egress(m, out_len ? *out_len : 0);
        if (m->pending.pending_emit >= m->pending.pending_cnt) {
            m->pending.pending_cnt = m->pending.pending_emit = 0;
        }
        return 1;
    }

    if (m->has_held) {
        return live_write_held(m, out, cap, out_len);
    }

    if (m->play.readers.gop && m->egress_pipe) {
        zms_egress_flv_tag tag;
        int r = zms_egress_pipeline_pull_flv_live(m->egress_pipe, &tag, 500, 0, NULL);

        if (r <= 0) {
            return r;
        }
        if (tag.msg_type == ZMS_RTMP_MSG_VIDEO && !m->logged_video) {
            m->logged_video = 1;
            zn_debug("[http] first_video ts=%u len=%u", (unsigned)tag.tag_dts_ms,
                      (unsigned)tag.len);
        }
        if (tag.msg_type == ZMS_RTMP_MSG_AUDIO &&
            zms_flv_mux_try_split_aac_tag(&m->pending, tag.body, tag.len, tag.tag_dts_ms)) {
            m->pending.pending_emit = 0;
            uint32_t out_ts =
                live_out_tag_dts_ms(m, ZMS_TRACK_AUDIO, m->pending.pending_tag_dts_ms[0]);
            if (!m->pending.pending_body[0] ||
                zms_flv_write_tag(out, cap, out_len, m->pending.pending_type[0], out_ts,
                                  m->pending.pending_body[0],
                                  m->pending.pending_len[0]) != ZN_OK) {
                return -1;
            }
            live_muxer_count_egress(m, out_len ? *out_len : 0);
            m->pending.pending_emit = 1;
            if (!m->logged_audio) {
                m->logged_audio = 1;
                zn_debug("[http] first_audio_split ts=%u", (unsigned)out_ts);
            }
            return 1;
        }
        if (zms_flv_write_tag(out, cap, out_len, tag.msg_type, tag.tag_dts_ms, tag.body, tag.len) !=
            ZN_OK) {
            return live_hold_tag(m, &tag, cap, out_len);
        }
        live_muxer_count_egress(m, out_len ? *out_len : 0);
        if (tag.msg_type == ZMS_RTMP_MSG_AUDIO && !m->logged_audio) {
            m->logged_audio = 1;
            zn_debug("[http] first_audio ts=%u len=%u", (unsigned)tag.tag_dts_ms,
                      (unsigned)tag.len);
        }
        return 1;
    }

    return 0;
}
