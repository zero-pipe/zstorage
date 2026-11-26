/**
 * @file session_publish.c
 * @brief RTMP 推流会话：publish 命令、音视频/脚本入站与录制。
 *
 * publish 鉴权在客户端 URL 上带 token，由 zms_auth 本地同步验签：
 * 验签通过才接入发布源，未通过直接回 BadAuth。全程不等待、不缓存媒体。
 */
#include "session/rtmp/session_internal.h"
#include "session/rtmp/session_transport.h"
#include "publish/rtmp/zm_ingest.h"
#include "zms/core/auth.h"
#include "zms/play/dispatch.h"
#include "zms/egress/recorder/egress_mp4_recorder.h"
#include "zms/core/event.h"
#include "zn/poller/poller.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

static void rtmp_publish_abort(zms_rtmp_session *s)
{
    if (s->ingress) {
        zms_live_ingest_destroy(s->ingress);
    }
    s->ingress = NULL;
    s->source = NULL;
}

/* 鉴权通过后接入发布源。失败时会清理 ingress。 */
static int rtmp_publish_commit(zms_rtmp_session *s)
{
    zms_session_publish_opts pubcfg;

    s->state = ZMS_RTMP_SESSION_STATE_PUBLISHING;
    s->zm_ingest = zms_rtmp_zm_ingest_create(s->ingress);
    if (!s->zm_ingest) {
        zn_error("RTMP #%u zm ingest create failed", s->session_no);
        s->state = ZMS_RTMP_SESSION_STATE_IDLE;
        rtmp_publish_abort(s);
        return -1;
    }
    memset(&pubcfg, 0, sizeof(pubcfg));
    pubcfg.schema = ZMS_SESSION_RTMP;
    if (zms_session_attach_publish(ZMS_SESSION_RTMP, s, s->source, &pubcfg) != ZN_OK) {
        zn_warn("RTMP #%u publish attach failed: app=%s stream=%s", s->session_no, s->app,
                 s->stream);
        zms_rtmp_zm_ingest_destroy((zms_rtmp_zm_ingest *)s->zm_ingest);
        s->zm_ingest = NULL;
        s->state = ZMS_RTMP_SESSION_STATE_IDLE;
        rtmp_publish_abort(s);
        return -1;
    }
    zms_media_event_publish(s->source, ZMS_ORIGIN_RTMP_PUSH);
    zms_media_source_set_publisher(s->source, s, zms_rtmp_session_publisher_kick);
    if (s->source->enable_mp4) {
        zn_poller *pol = s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
        (void)zms_mp4_recorder_start(s->source, pol);
    }

    zn_info("RTMP #%u publish start: app=%s client_stream=%s stream_id=%s enable_mp4=%d",
             s->session_no, s->app, s->stream, s->source->stream, s->source->enable_mp4);
    return 0;
}

/**
 * @return 0 已接受；-1 拒绝（含鉴权失败，客户端收到 NetStream.Publish.BadAuth）。
 */
int rtmp_srv_onpublish(void *param, double transaction, const char *app_name,
                       const char *stream_name, const char *type)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;
    zms_auth_request req;
    zms_auth_decision dec;
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char query[ZMS_AUTH_PARAMS_MAX];
    char ip[64];
    unsigned port = 0;

    (void)transaction;
    (void)type;
    if (!s) {
        return -1;
    }

    /* app/stream 名字上可能挂查询串（ffmpeg 的 publish 路径写法），
     * 摘出 '?' 之后作为鉴权参数，名字本身截断。 */
    zms_auth_split_query(app_name, app, sizeof(app), query, sizeof(query));
    if (query[0]) {
        zms_auth_params_copy(query, s->params, sizeof(s->params));
    }
    zms_auth_split_query(stream_name, stream, sizeof(stream), query, sizeof(query));
    if (query[0]) {
        zms_auth_params_copy(query, s->params, sizeof(s->params));
    }

    zms_rtmp_session_resolve_target(s, app, stream);
    s->ingress = zms_live_ingest_create_publish_vhost(s->vhost, s->app, s->stream, NULL);
    if (s->ingress && s->tcp) {
        zms_live_ingest_set_poller(s->ingress, zn_tcp_session_poller(s->tcp));
    }
    s->source = s->ingress ? zms_live_ingest_source(s->ingress) : NULL;
    if (!s->source) {
        zn_error("RTMP #%u publish failed: app=%s stream=%s", s->session_no, s->app, s->stream);
        rtmp_publish_abort(s);
        return -1;
    }

    /* 鉴权：上游签发的 token 在 URL 参数里，本地同步验签（不阻塞、不回调）。 */
    zms_auth_fill_peer(s->tcp, ip, sizeof(ip), &port);
    memset(&req, 0, sizeof(req));
    req.action = ZMS_AUTH_ACTION_PUBLISH;
    req.schema = ZMS_SCHEMA_RTMP;
    req.vhost = s->vhost;
    req.app = s->app;
    req.stream = s->stream;
    req.params = s->params[0] ? s->params : NULL;
    req.ip = ip[0] ? ip : NULL;
    req.port = port;
    if (!zms_auth_check(&req, &dec)) {
        zn_warn("RTMP #%u publish denied: app=%s stream=%s", s->session_no, s->app, s->stream);
        rtmp_publish_abort(s);
        return -1;
    }
    s->source->enable_mp4 = dec.enable_mp4;

    if (rtmp_publish_commit(s) != 0) {
        return -1;
    }
    return 0;
}

int rtmp_srv_onvideo(void *param, const void *data, size_t bytes, uint32_t timestamp)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;

    if (!s || s->state != ZMS_RTMP_SESSION_STATE_PUBLISHING || !s->ingress || !data || bytes < 2) {
        return 0;
    }

    if (bytes >= 2 && ((const uint8_t *)data)[1] == 0 && s->source && !s->source->has_video) {
        zn_info("RTMP #%u received AVC seq header: app=%s stream=%s bytes=%u", s->session_no,
                 s->app, s->stream, (unsigned)bytes);
    }

    if (s->zm_ingest) {
        return zms_rtmp_zm_ingest_input_tag((zms_rtmp_zm_ingest *)s->zm_ingest, 9, data, bytes,
                                             timestamp) == ZN_OK
                   ? 0
                   : -1;
    }
    return -1;
}

int rtmp_srv_onaudio(void *param, const void *data, size_t bytes, uint32_t timestamp)
{
    zms_rtmp_session *s = (zms_rtmp_session *)param;

    if (!s || s->state != ZMS_RTMP_SESSION_STATE_PUBLISHING || !s->ingress || !data || bytes < 2) {
        return 0;
    }

    if (bytes >= 2 && ((const uint8_t *)data)[1] == 0 && s->source && !s->source->has_audio) {
        zn_info("RTMP #%u received AAC seq header: app=%s stream=%s bytes=%u", s->session_no,
                 s->app, s->stream, (unsigned)bytes);
    }

    if (s->zm_ingest) {
        return zms_rtmp_zm_ingest_input_tag((zms_rtmp_zm_ingest *)s->zm_ingest, 8, data, bytes,
                                             timestamp) == ZN_OK
                   ? 0
                   : -1;
    }
    return -1;
}

int rtmp_srv_onscript(void *param, const void *data, size_t bytes, uint32_t timestamp)
{
    (void)param;
    (void)data;
    (void)bytes;
    (void)timestamp;
    return 0;
}
