/**
 * @file session.c
 * @brief RTMP 会话生命周期：TCP 收发、锁与销毁调度。
 */
#include "zms/session/rtmp/session.h"
#include "session/rtmp/session_internal.h"
#include "session/rtmp/session_transport.h"
#include "publish/rtmp/zm_ingest.h"
#include "zms/util/buf_pool.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller.h"
#include "zn/util/timer.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    zms_rtmp_session *s;
    unsigned token;
} rtmp_destroy_job;

static void zms_rtmp_session_destroy_task(void *user);

static uint64_t zms_rtmp_session_destroy_delayed(void *user)
{
    zms_rtmp_session_destroy_task(user);
    return 0;
}

static void zms_rtmp_session_destroy_task(void *user)
{
    rtmp_destroy_job *job = (rtmp_destroy_job *)user;
    zms_rtmp_session *s = job ? job->s : NULL;
    unsigned token = job ? job->token : 0;

    if (job) {
        free(job);
    }
    if (!s || s->destroy_token != token) {
        return;
    }
    zms_rtmp_session_destroy(s);
}

static unsigned g_rtmp_session_serial;

void zms_rtmp_session_lock(zms_rtmp_session *s)
{
    if (s && s->pl.mtx) {
        zb_mutex_lock(s->pl.mtx);
    }
}

void zms_rtmp_session_unlock(zms_rtmp_session *s)
{
    if (s && s->pl.mtx) {
        zb_mutex_unlock(s->pl.mtx);
    }
}

zms_rtmp_session *zms_rtmp_session_create(const zms_rtmp_session_opts *opts)
{
    zms_rtmp_session *s;

    if (!opts || !opts->tcp) {
        return NULL;
    }

    s = (zms_rtmp_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }

    s->tcp = opts->tcp;
    s->session_no = ++g_rtmp_session_serial;
    s->pl.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    s->stream_id = 1;
    s->receive_audio = 1;
    s->receive_video = 1;

    s->proto = zms_rtmp_transport_create(s);
    if (!s->proto) {
        zb_mutex_destroy(s->pl.mtx);
        free(s);
        return NULL;
    }
    zms_rtmp_protocol_set_poller(s->proto, zn_tcp_session_poller(opts->tcp));

    zn_info("RTMP #%u: session create", s->session_no);
    return s;
}

void zms_rtmp_session_destroy(zms_rtmp_session *s)
{
    if (!s) {
        return;
    }
    s->destroy_token++;
    s->pl.boot_pending = 0;
    s->destroy_scheduled = 1;
    zn_info("RTMP #%u: session destroy", s->session_no);
    zms_rtmp_session_lock(s);
    if (!s->destroy_scheduled) {
        zms_rtmp_session_teardown(s);
    }
    zms_rtmp_zm_ingest_destroy((zms_rtmp_zm_ingest *)s->zm_ingest);
    s->zm_ingest = NULL;
    zms_live_ingest_destroy(s->ingress);
    s->ingress = NULL;
    zms_rtmp_protocol_destroy(s->proto);
    s->proto = NULL;
    {
        zn_poller *pol = s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
        zms_buf_pool_slot_clear_poller(&s->pl.tag_buf, &s->pl.tag_cap, pol);
        zms_buf_pool_slot_clear_poller(&s->pl.es_buf, &s->pl.es_cap, pol);
    }
    if (s->pl.mtx) {
        zms_rtmp_session_unlock(s);
        zb_mutex_destroy(s->pl.mtx);
        s->pl.mtx = NULL;
    }
    free(s);
}

void zms_rtmp_session_on_recv(zms_rtmp_session *s, const void *data, size_t len)
{
    int r;
    int flush;

    if (!s || s->destroy_scheduled || !s->proto || !data || len == 0) {
        return;
    }
    zms_rtmp_session_lock(s);
    if (s->destroy_scheduled || !s->proto) {
        zms_rtmp_session_unlock(s);
        return;
    }
    r = (int)zms_rtmp_transport_input(s, data, len);
    if (r != 0) {
        zn_warn("RTMP #%u chunk input error: %d", s->session_no, r);
    }
    if (zms_rtmp_protocol_handshake_pending(s->proto)) {
        uint8_t hs[1 + 1536 * 2];
        size_t n = 0;
        if (zms_rtmp_protocol_send_handshake_s0s1s2(s->proto, hs, sizeof(hs), &n) == ZN_OK) {
            (void)rtmp_srv_send(s, hs, n, NULL, 0);
        }
    }
    if (zms_rtmp_protocol_handshake_complete(s->proto)) {
        uint8_t init[256];
        size_t n = 0;
        if (zms_rtmp_protocol_send_server_init(s->proto, init, sizeof(init), &n) == ZN_OK) {
            (void)rtmp_srv_send(s, init, n, NULL, 0);
        }
    }
    flush = !s->destroy_scheduled && s->state == ZMS_RTMP_SESSION_STATE_PLAYING &&
            !s->pl.boot_pending;
    zms_rtmp_session_unlock(s);
    if (flush) {
        zms_rtmp_session_play_flush(s);
    }
}

void zms_rtmp_session_on_error(zms_rtmp_session *s)
{
    zms_rtmp_session_schedule_destroy(s, NULL);
}

void zms_rtmp_session_schedule_destroy(zms_rtmp_session *s, zn_tcp_session *session)
{
    rtmp_destroy_job *job;
    zn_poller *pol;

    if (!s) {
        return;
    }
    zms_rtmp_session_lock(s);
    if (s->destroy_scheduled) {
        zms_rtmp_session_unlock(s);
        return;
    }
    zn_info("RTMP #%u: schedule_destroy tcp=%p", s->session_no, (void *)s->tcp);
    s->destroy_scheduled = 1;
    s->pl.boot_pending = 0;
    s->tcp = NULL;
    s->destroy_token++;
    zms_rtmp_session_teardown(s);
    zms_rtmp_session_unlock(s);

    job = (rtmp_destroy_job *)malloc(sizeof(*job));
    if (!job) {
        zms_rtmp_session_destroy(s);
        return;
    }
    job->s = s;
    job->token = s->destroy_token;

    pol = session ? zn_tcp_session_poller(session) : NULL;
    if (pol && zn_poller_do_delay(pol, 0, zms_rtmp_session_destroy_delayed, job)) {
        return;
    }
    zms_rtmp_session_destroy_task(job);
}

void zms_rtmp_session_on_manager(zms_rtmp_session *s)
{
    if (!s || s->destroy_scheduled || !s->tcp) {
        return;
    }
    if (s->state == ZMS_RTMP_SESSION_STATE_PLAYING && !s->pl.boot_pending) {
        zms_rtmp_session_play_flush(s);
    }
}
