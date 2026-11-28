/**
 * @file session.c
 * @brief SRT 会话生命周期与链接。
 */
#include "poller.h"
#include "zcs/protocol/srt/streamid.h"
#include "zcs/play/dispatch.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

static void srt_sock_nonblock(SRTSOCKET sock)
{
    int no = 0;

    if (sock == SRT_INVALID_SOCK) {
        return;
    }
    (void)srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof(no));
}

static void srt_log_negotiated_opts(zcs_srt_session *s)
{
    int lat = 0;
    int peer = 0;
    int plsize = 0;
    int len;

    if (!s || s->sock == SRT_INVALID_SOCK) {
        return;
    }
    len = (int)sizeof(lat);
    if (srt_getsockopt(s->sock, 0, SRTO_LATENCY, &lat, &len) != 0) {
        return;
    }
    len = (int)sizeof(peer);
    (void)srt_getsockopt(s->sock, 0, SRTO_PEERLATENCY, &peer, &len);
    len = (int)sizeof(plsize);
    (void)srt_getsockopt(s->sock, 0, SRTO_PAYLOADSIZE, &plsize, &len);
    zn_info("SRT #%d negotiated latency=%dms peer=%dms payload=%d", s->session_no, lat, peer,
             plsize);
}

static int srt_session_setup(zcs_srt_session *s, const char *streamid)
{
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    zcs_srt_stream_mode mode = ZCS_SRT_MODE_PUBLISH;

    if (!s) {
        return -1;
    }
    {
        char vh[ZCS_VHOST_MAX];

        if (zcs_srt_streamid_vhost(streamid, vh, sizeof(vh))) {
            zcs_vhost_normalize(vh, s->vhost, sizeof(s->vhost));
        } else {
            zcs_vhost_normalize(NULL, s->vhost, sizeof(s->vhost));
        }
    }
    if (zcs_srt_streamid_parse(streamid, app, stream, &mode) != 0) {
        zn_warn("SRT #%d bad streamid: %s", s->session_no, streamid ? streamid : "");
        return -1;
    }
    if (mode == ZCS_SRT_MODE_PLAY) {
        return zcs_srt_session_begin_play(s, app, stream, streamid);
    }
    if (mode != ZCS_SRT_MODE_PUBLISH) {
        zn_warn("SRT #%d invalid streamid mode: %s", s->session_no, streamid ? streamid : "");
        return -1;
    }
    return zcs_srt_session_begin_publish(s, app, stream, streamid);
}

static void srt_session_finish_publish_async(void *user)
{
    zcs_srt_session_finish_publish((zcs_srt_session *)user);
}

static void srt_session_finish_play_async(void *user)
{
    zcs_srt_session_finish_play((zcs_srt_session *)user);
}

static void srt_session_destroy_task(void *user)
{
    zcs_srt_session_destroy_now((zcs_srt_session *)user);
}

void zcs_srt_session_destroy_now(zcs_srt_session *s)
{
    const zcs_session_dispatch_ops *ops;
    zcs_srt_server *srv;

    if (!s) {
        return;
    }
    srv = s->server;
    if (s->destroy_scheduled) {
        return;
    }
    s->destroy_scheduled = 1;
    s->stopping = 1;

    if (srv && srv->epoll_id >= 0 && s->sock != SRT_INVALID_SOCK) {
        (void)srt_epoll_remove_usock(srv->epoll_id, s->sock);
    }
    if (s->sock != SRT_INVALID_SOCK) {
        srt_close(s->sock);
        s->sock = SRT_INVALID_SOCK;
    }

    ops = zcs_session_dispatch_find(ZCS_SESSION_SRT);
    if (ops && ops->on_teardown) {
        ops->on_teardown(s);
    }
    if (s->mode == ZCS_SRT_SESSION_MODE_PUBLISH) {
        zcs_srt_session_teardown_publish(s);
    } else {
        zcs_srt_session_teardown_play(s);
    }

    if (srv) {
        zcs_srt_session_unlink(srv, s);
    }
    if (s->ingress) {
        zcs_live_ingest_destroy(s->ingress);
        s->ingress = NULL;
    }
    s->source = NULL;
    s->publish_pipeline = NULL;
    free(s->recv_buf);
    s->recv_buf = NULL;
    free(s);
}

void zcs_srt_session_schedule_destroy(zcs_srt_session *sess)
{
    if (!sess || sess->destroy_scheduled) {
        return;
    }
    sess->stopping = 1;
    if (sess->poller && zn_poller_is_current_thread(sess->poller)) {
        zcs_srt_session_destroy_now(sess);
    } else if (sess->poller) {
        (void)zn_poller_async(sess->poller, srt_session_destroy_task, sess, 0);
    } else {
        zcs_srt_session_destroy_now(sess);
    }
}

void zcs_srt_session_publisher_kick(void *ctx, int force)
{
    zcs_srt_session *s = (zcs_srt_session *)ctx;

    (void)force;
    if (!s) {
        return;
    }
    s->stopping = 1;
    if (s->sock != SRT_INVALID_SOCK) {
        srt_close(s->sock);
    }
}

zcs_srt_session *zcs_srt_session_accept(zcs_srt_server *srv, SRTSOCKET client,
                                        const char *streamid)
{
    zcs_srt_session *s;
    int events = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR;

    s = (zcs_srt_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->server = srv;
    s->session_no = ++srv->session_serial;
    s->sock = client;
    s->poller = srv->poller;
    zcs_srt_apply_session_opts(client);
    srt_sock_nonblock(client);

    if (srt_session_setup(s, streamid) != 0) {
        free(s);
        return NULL;
    }
    if (s->mode == ZCS_SRT_SESSION_MODE_PUBLISH) {
        events = SRT_EPOLL_IN | SRT_EPOLL_ERR;
    }
    if (srt_epoll_add_usock(srv->epoll_id, client, &events) == SRT_ERROR) {
        const zcs_session_dispatch_ops *ops = zcs_session_dispatch_find(ZCS_SESSION_SRT);
        if (ops && ops->on_teardown) {
            ops->on_teardown(s);
        }
        if (s->mode == ZCS_SRT_SESSION_MODE_PUBLISH) {
            zcs_srt_session_teardown_publish(s);
        } else {
            zcs_srt_session_teardown_play(s);
        }
        if (s->ingress) {
            zcs_live_ingest_destroy(s->ingress);
        }
        free(s);
        return NULL;
    }
    zcs_srt_session_link(srv, s);
    srt_log_negotiated_opts(s);
    if (s->mode == ZCS_SRT_SESSION_MODE_PUBLISH) {
        zcs_srt_session_drain_recv(s);
        if (s->poller && zn_poller_is_current_thread(s->poller)) {
            zcs_srt_session_finish_publish(s);
        } else if (zn_poller_async(s->poller, srt_session_finish_publish_async, s, 0) != ZN_OK) {
            srt_session_finish_publish_async(s);
        }
    } else {
        zcs_srt_session_pump_send(s);
        if (s->poller && zn_poller_is_current_thread(s->poller)) {
            zcs_srt_session_finish_play(s);
        } else if (zn_poller_async(s->poller, srt_session_finish_play_async, s, 0) != ZN_OK) {
            srt_session_finish_play_async(s);
        }
    }
    return s;
}

zcs_srt_session *zcs_srt_session_find(zcs_srt_server *srv, SRTSOCKET sock)
{
    zcs_srt_session *s;

    if (!srv) {
        return NULL;
    }
    for (s = srv->sessions; s; s = s->next) {
        if (s->sock == sock) {
            return s;
        }
    }
    return NULL;
}

void zcs_srt_session_link(zcs_srt_server *srv, zcs_srt_session *sess)
{
    if (!srv || !sess) {
        return;
    }
    sess->next = srv->sessions;
    srv->sessions = sess;
}

void zcs_srt_session_unlink(zcs_srt_server *srv, zcs_srt_session *sess)
{
    zcs_srt_session **pp;

    if (!srv || !sess) {
        return;
    }
    for (pp = &srv->sessions; *pp; pp = &(*pp)->next) {
        if (*pp == sess) {
            *pp = sess->next;
            sess->next = NULL;
            return;
        }
    }
}
