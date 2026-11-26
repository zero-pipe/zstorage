/**
 * @file rtsp_server.c
 * @brief RTSP 服务端监听与会话创建/销毁。
 */
#include "zms/session/rtsp/rtsp_server.h"
#include "zms/play/dispatch.h"
#include "zms/play/play_binding.h"
#include "session/rtsp/session_internal.h"
#include "zms/session/rtsp/session_auth.h"
#include "zms/core/event.h"
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/core/stream/hub.h"
#include "zms/media/wire/rtp_packet.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller.h"
#include "zn/util/timer.h"
#include "zn/poller/poller_pool.h"
#include "zms/core/zms_atomic.h"
#include "zms/util/buf_pool.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

static uint64_t g_rtsp_session_serial;
/** SDP o= 对外公告地址；由 zms_rtsp_server_create 写入，session_create_user 读取 */
static char g_rtsp_advertise_host[64];

struct zms_rtsp_server {
    zn_tcp_server *tcp;
    zn_tcp_server *tcp_tls;
};

void zms_rtsp_session_publisher_kick(void *ctx, int force)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)ctx;
    (void)force;
    if (rs && rs->tcp) {
        zn_tcp_session_close(rs->tcp);
    }
}

void zms_rtsp_session_on_rtp(uint8_t channel, const uint8_t *data, size_t len, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !data || len < 8) {
        return;
    }

    if (rs->mode == ZMS_RTSP_SESSION_MODE_PLAY) {
        if (zms_rtp_is_rtcp(data, len)) {
            zms_rtsp_session_play_on_rtcp(rs, data, len);
        }
        return;
    }

    if (rs->mode != ZMS_RTSP_SESSION_MODE_RECORD || len < 12) {
        return;
    }
    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }

    int tidx = zms_rtsp_session_record_track_by_channel(rs, channel);
    if (tidx < 0) {
        return;
    }
    zms_rtsp_session_record_input_rtp_raw(rs, tidx, data, len);
}

void zms_rtsp_session_teardown(zms_rtsp_session *rs)
{
    zms_media_source *src;
    int record;
    zms_play_binding bind;

    if (!rs) {
        return;
    }
    src = rs->source;
    record = (rs->mode == ZMS_RTSP_SESSION_MODE_RECORD);

    zms_session_detach_play(ZMS_SESSION_RTSP, rs);
    memset(&bind, 0, sizeof(bind));
    bind.source = &rs->source;
    bind.gop_reader = &rs->pl.gop_reader;
    bind.reader_attached = &rs->pl.reader_attached;
    bind.play_start_ms = &rs->play_start_ms;
    bind.player = "rtsp";
    if (bind.gop_reader) {
        *bind.gop_reader = NULL;
    }
    zms_play_binding_reader_stop(&bind);
    if (record && rs->splitter) {
        zms_rtsp_splitter_enable_rtp(rs->splitter, 0);
    }
    if (record && src) {
        int linger = zms_media_source_linger(src, ZMS_ORIGIN_RTSP_PUSH);

        if (!linger && rs->pb.ingress) {
            zms_live_ingest_reset(rs->pb.ingress);
        }
        zms_media_source_clear_publisher(src, rs);
        if (!linger) {
            zms_media_event_publish_fini(src, ZMS_ORIGIN_RTSP_PUSH);
        }
    } else if (record && rs->pb.ingress) {
        zms_live_ingest_reset(rs->pb.ingress);
    }
}

static void zms_rtsp_session_close(zms_rtsp_session *rs)
{
    if (!rs || !rs->tcp || rs->destroy_scheduled) {
        return;
    }
    zn_tcp_session *tcp = rs->tcp;
    rs->tcp = NULL;
    zn_tcp_session_close(tcp);
}

static void zms_rtsp_session_destroy_task(void *user);

static uint64_t zms_rtsp_session_destroy_delayed(void *user)
{
    zms_rtsp_session_destroy_task(user);
    return 0;
}

/* 真正释放会话。调用方须保证 rs 有效，且不再通过其他路径访问 rs。 */
static void zms_rtsp_session_destroy_core(zms_rtsp_session *rs, unsigned token)
{
    if (!rs) {
        return;
    }
    if (rs->destroy_token != token) {
        zn_info("RTSP: destroy skip stale rs=%p token=%u cur=%u", (void *)rs, token,
                 rs->destroy_token);
        return;
    }
    {
        const zms_rtp_muxer_stats *st = zms_rtp_muxer_get_stats(rs->pl.rtp_muxer);
        zn_info("RTSP #%u: destroy rs=%p mode=%d rtpq=%zu v_pkts=%u a_pkts=%u", rs->session_no,
                 (void *)rs, rs->mode, zms_rtp_play_sender_pending(rs->pl.sender),
                 st ? st->video_pkt_count : 0, st ? st->audio_pkt_count : 0);
    }
    zms_session_detach_play(ZMS_SESSION_RTSP, rs);
    zms_rtp_play_sender_destroy(rs->pl.sender);
    rs->pl.sender = NULL;
    zms_rtsp_session_play_mux_destroy(rs);
    zms_rtsp_session_udp_teardown(rs);
    zms_rtsp_session_record_payload_teardown(rs);
    zms_live_ingest_destroy(rs->pb.ingress);
    zms_rtsp_splitter_destroy(rs->splitter);
    if (rs->poller) {
        zms_buf_pool_slot_clear_poller(&rs->resp_buf, &rs->resp_cap, rs->poller);
        zms_buf_pool_slot_clear_poller(&rs->pl.interleaved_buf, &rs->play_interleaved_cap,
                                       rs->poller);
    } else {
        zms_buf_pool_slot_clear(&rs->resp_buf, &rs->resp_cap);
        zms_buf_pool_slot_clear(&rs->pl.interleaved_buf, &rs->play_interleaved_cap);
    }
    free(rs);
}

static void zms_rtsp_session_destroy_task(void *user)
{
    zms_rtsp_destroy_job *job = (zms_rtsp_destroy_job *)user;
    zms_rtsp_session *rs = job ? job->rs : NULL;
    unsigned token = job ? job->token : 0;
    if (job) {
        free(job);
    }
    zms_rtsp_session_destroy_core(rs, token);
}

void zms_rtsp_session_schedule_destroy(zms_rtsp_session *rs, zn_tcp_session *session)
{
    zn_poller *pol;
    zms_rtsp_destroy_job *job;

    if (!rs || rs->destroy_scheduled) {
        return;
    }
    zn_info("RTSP #%u: schedule_destroy rs=%p mode=%d es=%d readers=%d rtpq=%zu tcp=%p",
             rs->session_no, (void *)rs, rs->mode, rs->pl.gop_reader ? 1 : 0,
             rs->source ? zms_media_source_reader_count(rs->source) : 0,
             zms_rtp_play_sender_pending(rs->pl.sender), (void *)rs->tcp);
    rs->destroy_scheduled = 1;
    rs->close_pending = 1;
    rs->tcp = NULL;
    rs->destroy_token++;
    zms_rtsp_session_teardown(rs);

    job = (zms_rtsp_destroy_job *)malloc(sizeof(*job));
    if (!job) {
        zms_rtsp_session_destroy_core(rs, rs->destroy_token);
        return;
    }
    job->rs = rs;
    job->token = rs->destroy_token;

    pol = session ? zn_tcp_session_poller(session) : rs->poller;
    if (pol && zn_poller_do_delay(pol, 0, zms_rtsp_session_destroy_delayed, job)) {
        return;
    }
    zms_rtsp_session_destroy_task(job);
}

static void on_recv(zn_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !rs->splitter || rs->destroy_scheduled) {
        return;
    }
    zms_rtsp_splitter_input(rs->splitter, data, len);
    if (rs->close_pending && !rs->destroy_scheduled) {
        zms_rtsp_session_close(rs);
    }
}

static void on_manager(zn_tcp_session *session, void *user)
{
    (void)session;
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || !rs->tcp || rs->close_pending || rs->destroy_scheduled ||
        rs->mode != ZMS_RTSP_SESSION_MODE_PLAY) {
        return;
    }
    if (!rs->pl.boot_sent) {
        zms_rtsp_session_play_kick(rs);
    } else {
        zms_rtsp_session_play_tick(rs);
    }
    if (rs->close_pending || rs->destroy_scheduled) {
        return;
    }
    if (++rs->pl.rtcp_tick >= 250) {
        zms_rtsp_session_send_rtcp_srs(rs);
        rs->pl.rtcp_tick = 0;
    }
    zn_tcp_session_flush(rs->tcp);
}

static void on_error(zn_tcp_session *session, void *user)
{
    zms_rtsp_session *rs = (zms_rtsp_session *)user;
    if (!rs || rs->destroy_scheduled) {
        return;
    }
    zms_rtsp_session_schedule_destroy(rs, session);
}

static void *session_create_user(zn_tcp_server *srv, zn_tcp_session *session)
{
    (void)srv;
    zms_rtsp_session *rs = (zms_rtsp_session *)calloc(1, sizeof(*rs));
    if (!rs) {
        return NULL;
    }
    rs->session_no = (unsigned)(ZMS_ATOMIC_ADD64(&g_rtsp_session_serial, 1) + 1);
    rs->tcp = session;
    rs->poller = zn_tcp_session_poller(session);
    if (g_rtsp_advertise_host[0]) {
        strncpy(rs->advertise_host, g_rtsp_advertise_host, sizeof(rs->advertise_host) - 1);
    }
    zn_info("RTSP #%u: session create rs=%p", rs->session_no, (void *)rs);
    rs->pb.audio_rate = 44100;
    rs->pb.audio_channels = 2;
    rs->rtp_mode = ZMS_RTSP_RTP_TCP;
    rs->pl.video_rtp_ssrc = 0x12345678u;
    rs->pl.audio_rtp_ssrc = 0x87654321u;
    rs->pl.scale = 1.0;
    zms_rtsp_udp_registry_init();
    zms_rtsp_splitter_opts opts = {zms_rtsp_session_on_message, zms_rtsp_session_on_rtp, rs};
    rs->splitter = zms_rtsp_splitter_create(&opts);
    if (!rs->splitter) {
        free(rs);
        return NULL;
    }
    rs->low_latency = zms_rtsp_low_latency_enabled();
    if (rs->low_latency) {
        zn_info("RTSP #%u lowLatency=1 (pacing disabled)", rs->session_no);
    }
    rs->pl.sender =
        zms_rtp_play_sender_create(zms_rtsp_session_write_interleaved, rs, ZMS_RTP_PLAY_RTPQ_CAP);
    if (!rs->pl.sender) {
        zms_rtsp_splitter_destroy(rs->splitter);
        free(rs);
        return NULL;
    }
    zms_rtp_play_sender_set_session_no(rs->pl.sender, rs->session_no);
    zms_rtp_play_sender_set_poller(rs->pl.sender, rs->poller);
    return rs;
}

zms_rtsp_server *zms_rtsp_server_create(const zms_rtsp_server_opts *opts)
{
    if (!opts || !opts->poller_pool) {
        return NULL;
    }
    zms_session_dispatch_register_all();
    zms_rtsp_server *srv = (zms_rtsp_server *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    memset(g_rtsp_advertise_host, 0, sizeof(g_rtsp_advertise_host));
    if (opts->advertise_host && opts->advertise_host[0]) {
        strncpy(g_rtsp_advertise_host, opts->advertise_host, sizeof(g_rtsp_advertise_host) - 1);
    }

    zn_tcp_session_ops_t ops = {on_recv, on_error, on_manager};
    zn_tcp_server_opts_t topts = {
        .host = opts->host ? opts->host : "0.0.0.0",
        .port = opts->port ? opts->port : 554,
        .backlog = 64,
        .poller_pool = opts->poller_pool,
        .session_ops = &ops,
        .session_create_user = session_create_user,
        .manager_interval_sec = 0.02f,
        .handshake_timeout_ms = opts->handshake_timeout_ms,
        .idle_timeout_ms = opts->idle_timeout_ms,
        .max_sessions = opts->max_sessions,
        .max_sessions_per_ip = opts->max_sessions_per_ip,
    };
    srv->tcp = zn_tcp_server_create(&topts);
    if (!srv->tcp) {
        free(srv);
        return NULL;
    }
    if (opts->ssl_ctx && opts->ssl_port) {
        zn_tcp_server_opts_t tlsopts = topts;
        tlsopts.port = opts->ssl_port;
        tlsopts.ssl_ctx = opts->ssl_ctx;
        srv->tcp_tls = zn_tcp_server_create(&tlsopts);
        if (!srv->tcp_tls) {
            zn_warn("RTSPS listen create failed port=%u", (unsigned)opts->ssl_port);
        } else {
            zn_info("RTSPS listen %s:%u", topts.host ? topts.host : "0.0.0.0",
                     (unsigned)opts->ssl_port);
        }
    }
    return srv;
}

void zms_rtsp_server_destroy(zms_rtsp_server *srv)
{
    if (!srv) {
        return;
    }
    zn_tcp_server_destroy(srv->tcp);
    zn_tcp_server_destroy(srv->tcp_tls);
    free(srv);
}

zn_err_t zms_rtsp_server_start(zms_rtsp_server *srv)
{
    zn_err_t err;

    if (!srv || !srv->tcp) {
        return ZN_ERR_INVALID;
    }
    err = zn_tcp_server_start(srv->tcp);
    if (err != ZN_OK) {
        return err;
    }
    if (srv->tcp_tls) {
        err = zn_tcp_server_start(srv->tcp_tls);
        if (err != ZN_OK) {
            zn_warn("RTSPS listen failed: %s", zn_strerror(err));
        }
    }
    return ZN_OK;
}

void zms_rtsp_server_stop(zms_rtsp_server *srv)
{
    if (srv && srv->tcp) {
        zn_tcp_server_stop(srv->tcp);
    }
    if (srv && srv->tcp_tls) {
        zn_tcp_server_stop(srv->tcp_tls);
    }
}

uint16_t zms_rtsp_server_port(const zms_rtsp_server *srv)
{
    if (!srv || !srv->tcp) {
        return 0;
    }
    return zn_tcp_server_port(srv->tcp);
}
