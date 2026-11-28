/**
 * @file session.c
 * @brief RTMP 会话生命周期：TCP 收发、锁与销毁调度。
 */
#include "zcs/session/rtmp/session.h"
#include "session/rtmp/session_internal.h"
#include "session/rtmp/session_transport.h"
#include "publish/rtmp/zm_ingest.h"
#include "zcs/util/buf_pool.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller.h"
#include "zn/util/timer.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    zcs_rtmp_session *s;
    unsigned token;
} rtmp_destroy_job;

static void zcs_rtmp_session_destroy_task(void *user);

static uint64_t zcs_rtmp_session_destroy_delayed(void *user)
{
    zcs_rtmp_session_destroy_task(user);
    return 0;
}

static void zcs_rtmp_session_destroy_task(void *user)
{
    rtmp_destroy_job *job = (rtmp_destroy_job *)user;
    zcs_rtmp_session *s = job ? job->s : NULL;
    unsigned token = job ? job->token : 0;

    if (job) {
        free(job);
    }
    if (!s || s->destroy_token != token) {
        return;
    }
    zcs_rtmp_session_destroy(s);
}

static unsigned g_rtmp_session_serial;

void zcs_rtmp_session_lock(zcs_rtmp_session *s)
{
    if (s && s->pl.mtx) {
        zb_mutex_lock(s->pl.mtx);
    }
}

void zcs_rtmp_session_unlock(zcs_rtmp_session *s)
{
    if (s && s->pl.mtx) {
        zb_mutex_unlock(s->pl.mtx);
    }
}

zcs_rtmp_session *zcs_rtmp_session_create(const zcs_rtmp_session_opts *opts)
{
    zcs_rtmp_session *s;

    if (!opts || !opts->tcp) {
        return NULL;
    }

    s = (zcs_rtmp_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }

    s->tcp = opts->tcp;
    s->session_no = ++g_rtmp_session_serial;
    s->pl.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    s->stream_id = 1;
    s->receive_audio = 1;
    s->receive_video = 1;

    s->proto = zcs_rtmp_transport_create(s);
    if (!s->proto) {
        zb_mutex_destroy(s->pl.mtx);
        free(s);
        return NULL;
    }
    zcs_rtmp_protocol_set_poller(s->proto, zn_tcp_session_poller(opts->tcp));

    zn_info("RTMP #%u: session create", s->session_no);
    return s;
}

void zcs_rtmp_session_destroy(zcs_rtmp_session *s)
{
    if (!s) {
        return;
    }
    s->destroy_token++;
    s->pl.boot_pending = 0;
    s->destroy_scheduled = 1;
    zn_info("RTMP #%u: session destroy", s->session_no);
    zcs_rtmp_session_lock(s);
    if (!s->destroy_scheduled) {
        zcs_rtmp_session_teardown(s);
    }
    zcs_rtmp_zm_ingest_destroy((zcs_rtmp_zm_ingest *)s->zm_ingest);
    s->zm_ingest = NULL;
    zcs_live_ingest_destroy(s->ingress);
    s->ingress = NULL;
    zcs_rtmp_protocol_destroy(s->proto);
    s->proto = NULL;
    {
        zn_poller *pol = s->tcp ? zn_tcp_session_poller(s->tcp) : NULL;
        zcs_buf_pool_slot_clear_poller(&s->pl.tag_buf, &s->pl.tag_cap, pol);
        zcs_buf_pool_slot_clear_poller(&s->pl.es_buf, &s->pl.es_cap, pol);
    }
    if (s->pl.mtx) {
        zcs_rtmp_session_unlock(s);
        zb_mutex_destroy(s->pl.mtx);
        s->pl.mtx = NULL;
    }
    free(s);
}

void zcs_rtmp_session_on_recv(zcs_rtmp_session *s, const void *data, size_t len)
{
    int r;
    int flush;

    if (!s || s->destroy_scheduled || !s->proto || !data || len == 0) {
        return;
    }
    zcs_rtmp_session_lock(s);
    if (s->destroy_scheduled || !s->proto) {
        zcs_rtmp_session_unlock(s);
        return;
    }
    r = (int)zcs_rtmp_transport_input(s, data, len);
    if (r != 0) {
        zn_warn("RTMP #%u chunk input error: %d", s->session_no, r);
    }
    if (zcs_rtmp_protocol_handshake_pending(s->proto)) {
        uint8_t hs[1 + 1536 * 2];
        size_t n = 0;
        if (zcs_rtmp_protocol_send_handshake_s0s1s2(s->proto, hs, sizeof(hs), &n) == ZN_OK) {
            (void)rtmp_srv_send(s, hs, n, NULL, 0);
        }
    }
    if (zcs_rtmp_protocol_handshake_complete(s->proto)) {
        uint8_t init[256];
        size_t n = 0;
        if (zcs_rtmp_protocol_send_server_init(s->proto, init, sizeof(init), &n) == ZN_OK) {
            (void)rtmp_srv_send(s, init, n, NULL, 0);
        }
    }
    flush = !s->destroy_scheduled && s->state == ZCS_RTMP_SESSION_STATE_PLAYING &&
            !s->pl.boot_pending;
    zcs_rtmp_session_unlock(s);
    if (flush) {
        zcs_rtmp_session_play_flush(s);
    }
}

void zcs_rtmp_session_on_error(zcs_rtmp_session *s)
{
    zcs_rtmp_session_schedule_destroy(s, NULL);
}

void zcs_rtmp_session_schedule_destroy(zcs_rtmp_session *s, zn_tcp_session *session)
{
    rtmp_destroy_job *job;
    zn_poller *pol;

    if (!s) {
        return;
    }
    zcs_rtmp_session_lock(s);
    if (s->destroy_scheduled) {
        zcs_rtmp_session_unlock(s);
        return;
    }
    zn_info("RTMP #%u: schedule_destroy tcp=%p", s->session_no, (void *)s->tcp);
    s->destroy_scheduled = 1;
    s->pl.boot_pending = 0;
    s->tcp = NULL;
    s->destroy_token++;
    zcs_rtmp_session_teardown(s);
    zcs_rtmp_session_unlock(s);

    job = (rtmp_destroy_job *)malloc(sizeof(*job));
    if (!job) {
        zcs_rtmp_session_destroy(s);
        return;
    }
    job->s = s;
    job->token = s->destroy_token;

    pol = session ? zn_tcp_session_poller(session) : NULL;
    if (pol && zn_poller_do_delay(pol, 0, zcs_rtmp_session_destroy_delayed, job)) {
        return;
    }
    zcs_rtmp_session_destroy_task(job);
}

void zcs_rtmp_session_on_manager(zcs_rtmp_session *s)
{
    if (!s || s->destroy_scheduled || !s->tcp) {
        return;
    }
    if (s->state == ZCS_RTMP_SESSION_STATE_PLAYING && !s->pl.boot_pending) {
        zcs_rtmp_session_play_flush(s);
    }
}
