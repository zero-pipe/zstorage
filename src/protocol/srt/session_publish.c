/**
 * @file session_publish.c
 * @brief SRT 推流：接收/排空与入站。
 */
#include "protocol/srt/server_internal.h"
#include "zcs/core/auth.h"
#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include "zcs/core/event.h"
#include "zcs/play/dispatch.h"
#include "zn/util/log.h"
#include <string.h>

#define ZCS_SRT_RECV_BUF 65536

/* streamid 里的查询串即鉴权参数；SRT 用 ',' 分隔键值，转成 '&' 后按 query 解析。 */
static void srt_capture_params(zcs_srt_session *s, const char *streamid)
{
    char params[ZCS_AUTH_PARAMS_MAX];
    char *p;

    if (!s || !streamid) {
        return;
    }
    zcs_auth_split_query(streamid, NULL, 0, params, sizeof(params));
    if (!params[0]) {
        return;
    }
    for (p = params; *p; ++p) {
        if (*p == ',') {
            *p = '&';
        }
    }
    zcs_auth_params_copy(params, s->params, sizeof(s->params));
}

static int srt_publish_attach(zcs_srt_session *s)
{
    zcs_session_publish_opts pubcfg;

    memset(&pubcfg, 0, sizeof(pubcfg));
    pubcfg.schema = ZCS_SESSION_SRT;
    if (zcs_session_attach_publish(ZCS_SESSION_SRT, s, s->source, &pubcfg) != ZN_OK) {
        zn_warn("SRT #%d attach publish failed: app=%s stream=%s", s->session_no, s->app, s->stream);
        return -1;
    }
    return 0;
}

int zcs_srt_session_begin_publish(zcs_srt_session *s, const char *app, const char *stream,
                                  const char *streamid)
{
    zcs_auth_request req;
    zcs_auth_decision dec;

    if (!s || !app[0] || !stream[0]) {
        return -1;
    }

    s->mode = ZCS_SRT_SESSION_MODE_PUBLISH;
    strncpy(s->app, app, sizeof(s->app) - 1);
    strncpy(s->stream, stream, sizeof(s->stream) - 1);
    srt_capture_params(s, streamid);

    /* 推流鉴权：上游签发的 token 在 streamid 里，本地同步验签。 */
    memset(&req, 0, sizeof(req));
    req.action = ZCS_AUTH_ACTION_PUBLISH;
    req.schema = ZCS_SCHEMA_SRT;
    req.vhost = s->vhost;
    req.app = s->app;
    req.stream = s->stream;
    req.params = s->params[0] ? s->params : NULL;
    req.id = streamid;
    if (!zcs_auth_check(&req, &dec)) {
        zn_warn("SRT #%d publish denied: app=%s stream=%s", s->session_no, app, stream);
        return -1;
    }

    s->ingress = zcs_live_ingest_create_publish_schema_vhost(ZCS_SCHEMA_SRT, s->vhost, app,
                                                             stream, NULL);
    if (s->ingress) {
        zcs_live_ingest_set_poller(s->ingress, s->poller);
    }
    s->source = s->ingress ? zcs_live_ingest_source(s->ingress) : NULL;
    if (!s->source) {
        zn_error("SRT #%d publish failed: app=%s stream=%s", s->session_no, app, stream);
        return -1;
    }

    s->source->enable_mp4 = dec.enable_mp4;
    if (srt_publish_attach(s) != 0) {
        zcs_live_ingest_destroy(s->ingress);
        s->ingress = NULL;
        s->source = NULL;
        return -1;
    }
    return 0;
}

void zcs_srt_session_teardown_publish(zcs_srt_session *s)
{
    zcs_media_source *src;

    if (!s) {
        return;
    }
    src = s->source;
    {
        int linger = src ? zcs_media_source_linger(src, ZCS_ORIGIN_SRT_PUSH) : 0;

        if (!linger && s->ingress) {
            zcs_live_ingest_reset(s->ingress);
        }
        if (src) {
            zcs_media_source_clear_publisher(src, s);
            if (!linger) {
                zcs_media_event_publish_fini(src, ZCS_ORIGIN_SRT_PUSH);
            }
        }
    }
}

void zcs_srt_session_finish_publish(zcs_srt_session *s)
{
    if (!s || s->stopping || s->destroy_scheduled || !s->source) {
        return;
    }
    zcs_media_event_publish(s->source, ZCS_ORIGIN_SRT_PUSH);
    zcs_media_source_set_publisher(s->source, s, zcs_srt_session_publisher_kick);
    if (s->source->enable_mp4) {
        (void)zcs_mp4_recorder_start(s->source, s->poller);
    }
    zn_info("SRT #%d publish start: app=%s stream=%s stream_id=%s enable_mp4=%d", s->session_no,
             s->app, s->stream, s->source->stream, s->source->enable_mp4);
}

static void srt_session_feed_ts(zcs_srt_session *s, const uint8_t *buf, int n)
{
    if (!s || !buf || n <= 0 || !s->publish_pipeline || s->stopping || s->destroy_scheduled) {
        return;
    }
    (void)zcs_demux_pipeline_feed(s->publish_pipeline, buf, (size_t)n);
}

void zcs_srt_session_drain_recv(zcs_srt_session *s)
{
    /* 64KB 收包缓冲挂在会话上（堆），不放在协议线程栈。 */
    uint8_t *buf;
    SRT_SOCKSTATUS st;
    int total = 0;

    if (!s || s->stopping || s->destroy_scheduled || s->mode != ZCS_SRT_SESSION_MODE_PUBLISH) {
        return;
    }
    st = srt_getsockstate(s->sock);
    if (st == SRTS_BROKEN || st == SRTS_NONEXIST || st == SRTS_CLOSED) {
        zn_info("SRT #%d disconnected state=%d bytes=%llu", s->session_no, (int)st,
                 (unsigned long long)s->recv_bytes);
        zcs_srt_session_schedule_destroy(s);
        return;
    }

    if (!s->recv_buf) {
        s->recv_buf = (uint8_t *)malloc(ZCS_SRT_RECV_BUF);
        if (!s->recv_buf) {
            return;
        }
    }
    buf = s->recv_buf;

    for (;;) {
        int n = srt_recvmsg(s->sock, (char *)buf, ZCS_SRT_RECV_BUF);
        if (n > 0) {
            s->recv_bytes += (uint64_t)n;
            s->recv_calls++;
            s->last_recv_bytes = s->recv_bytes;
            s->stall_polls = 0;
            srt_session_feed_ts(s, buf, n);
            ++total;
            if (!s->logged_recv) {
                s->logged_recv = 1;
                zn_info("SRT #%d first recv %d bytes", s->session_no, n);
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (n == SRT_ERROR && srt_getlasterror(NULL) == SRT_EASYNCRCV) {
            break;
        }
        zn_info("SRT #%d recv end err=%s pkts=%d bytes=%llu", s->session_no,
                 srt_getlasterror_str(), total, (unsigned long long)s->recv_bytes);
        zcs_srt_session_schedule_destroy(s);
        return;
    }
}
