/**
 * @file session_play.c
 * @brief SRT 播放：发送 pump。
 */
#include "protocol/srt/server_internal.h"
#include "zms/egress/mpegts/mpegts_egress.h"
#include "zms/play/play_binding.h"
#include "zn/platform.h"
#include "zms/core/auth.h"
#include "zms/core/event.h"
#include "zms/core/codec_filter.h"
#include "zms/play/dispatch.h"
#include "zn/util/log.h"
#include <string.h>
#define ZMS_SRT_SEND_PAYLOAD 1316
#define ZMS_SRT_PLAY_SEND_ROUNDS 32

/* streamid 里的查询串即鉴权参数；SRT 用 ',' 分隔键值，转成 '&' 后按 query 解析。 */
static void srt_play_capture_params(zms_srt_session *s, const char *streamid)
{
    char params[ZMS_AUTH_PARAMS_MAX];
    char *p;

    if (!s || !streamid) {
        return;
    }
    zms_auth_split_query(streamid, NULL, 0, params, sizeof(params));
    if (!params[0]) {
        return;
    }
    for (p = params; *p; ++p) {
        if (*p == ',') {
            *p = '&';
        }
    }
    zms_auth_params_copy(params, s->params, sizeof(s->params));
}

int zms_srt_session_begin_play(zms_srt_session *s, const char *app, const char *stream,
                               const char *streamid)
{
    zms_session_play_opts pcfg;
    zms_auth_request req;
    zms_auth_decision dec;

    if (!s || !app[0] || !stream[0]) {
        return -1;
    }
    s->source = zms_media_source_find_for_play_ex(ZMS_SCHEMA_SRT, s->vhost, app, stream);
    if (!s->source || !s->source->gop_queue) {
        zn_warn("SRT #%d play 404: app=%s stream=%s", s->session_no, app, stream);
        return -1;
    }
    if (!zms_media_source_use_gop_queue_play(s->source)) {
        zn_warn("SRT #%d play unavailable: app=%s stream=%s", s->session_no, app, stream);
        return -1;
    }
    if (zms_session_capability_check_source(ZMS_PROTO_CAP_SRT_PLAY, s->source) != ZN_OK) {
        zms_session_capability_log_reject("srt", s->source, ZMS_PROTO_CAP_SRT_PLAY);
        return -1;
    }
    srt_play_capture_params(s, streamid);
    memset(&req, 0, sizeof(req));
    req.action = ZMS_AUTH_ACTION_PLAY;
    req.schema = ZMS_SCHEMA_SRT;
    req.vhost = s->vhost;
    req.app = app;
    req.stream = stream;
    req.params = s->params[0] ? s->params : NULL;
    req.id = streamid;
    if (!zms_auth_check(&req, &dec)) {
        zn_warn("SRT #%d play denied: app=%s stream=%s", s->session_no, app, stream);
        return -1;
    }
    s->mode = ZMS_SRT_SESSION_MODE_PLAY;
    strncpy(s->app, app, sizeof(s->app) - 1);
    strncpy(s->stream, stream, sizeof(s->stream) - 1);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZMS_SESSION_SRT;
    if (zms_session_attach_play(ZMS_SESSION_SRT, s, s->source, &pcfg) != ZN_OK) {
        zn_warn("SRT #%d attach play failed: app=%s stream=%s", s->session_no, app, stream);
        return -1;
    }
    s->live_muxer = zms_mpegts_egress_create(s->source, &s->play);
    if (!s->live_muxer) {
        zms_session_detach_play(ZMS_SESSION_SRT, s);
        return -1;
    }
    zms_media_source_reader_add(s->source);
    s->reader_attached = 1;
    s->play_start_ms = zn_monotonic_ms();
    zms_media_event_play(s->source, "srt");
    return 0;
}

void zms_srt_session_teardown_play(zms_srt_session *s)
{
    zms_play_binding bind;

    if (!s) {
        return;
    }
    memset(&bind, 0, sizeof(bind));
    bind.source = &s->source;
    bind.reader_attached = &s->reader_attached;
    bind.play_start_ms = &s->play_start_ms;
    bind.player = "srt";
    zms_play_binding_reader_stop(&bind);
}

void zms_srt_session_finish_play(zms_srt_session *s)
{
    if (!s || s->stopping || s->destroy_scheduled) {
        return;
    }
    zn_info("SRT #%d play start: app=%s stream=%s video=%d audio=%d", s->session_no, s->app,
             s->stream, s->source ? s->source->has_video : 0, s->source ? s->source->has_audio : 0);
}

void zms_srt_session_pump_send(zms_srt_session *s)
{
    uint8_t buf[ZMS_SRT_SEND_PAYLOAD];
    size_t n;
    int rounds = 0;
    if (!s || s->stopping || s->destroy_scheduled || s->mode != ZMS_SRT_SESSION_MODE_PLAY ||
        !s->live_muxer) {
        return;
    }
    while (rounds++ < ZMS_SRT_PLAY_SEND_ROUNDS) {
        int r = zms_mpegts_egress_next(s->live_muxer, buf, sizeof(buf), &n);
        if (r <= 0 || n == 0) {
            break;
        }
        {
            int sent = srt_sendmsg2(s->sock, (const char *)buf, (int)n, NULL);
            if (sent == SRT_ERROR) {
                int err = srt_getlasterror(NULL);
                if (err == SRT_EASYNCSND) {
                    break;
                }
                zn_info("SRT #%d play send err=%s sent=%llu", s->session_no,
                         srt_getlasterror_str(), (unsigned long long)s->send_bytes);
                zms_srt_session_schedule_destroy(s);
                return;
            }
            s->send_bytes += (uint64_t)sent;
            if (!s->logged_send) {
                s->logged_send = 1;
                zn_info("SRT #%d play first send %d bytes app=%s stream=%s", s->session_no, sent,
                         s->app, s->stream);
            }
        }
    }
}
