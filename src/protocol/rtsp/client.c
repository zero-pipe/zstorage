/**
 * @file client.c
 * @brief RTSP 拉流客户端：DESCRIBE/SETUP/PLAY 状态机与 RTP 回调。
 *
 * 状态机（单向推进，失败统一进 ST_ERROR 后按退避重连）：
 *   ST_IDLE → CONNECTING → OPTIONS → DESCRIBE → SETUP(×N 轨) → PLAY → PLAYING
 * 每一步都是一个“发请求 → 收到 200 → 推进”的回合；DESCRIBE 拿到 SDP 后
 * 按 track 数量逐个 SETUP，全部 SETUP 成功才发 PLAY。
 *
 * 线程模型：全部逻辑跑在创建时传入的 poller 线程上，socket 回调、定时器
 * 回调都在同一线程串行，故 client 内部字段无锁。唯一的异步是重连/延迟关闭
 * 定时器，它们也被投递回同一 poller。
 */
#include "protocol/rtsp/client_internal.h"
#include "bridge/zm_rtp_ingest.h"
#include "bridge/zm_zcs_bridge.h"
#include "zcs/ops/config/zcs_have_tls.h"
#include "zcs/media/media_limits.h"
#include "zcs/core/gop/limits.h"
#include "zcs/protocol/rtsp/digest.h"
#include "zcs/protocol/rtsp/rtsp.h"
#include "zn/util/timer.h"
#include "zn/util/log.h"
#include "zcs/core/module_registry.h"
#include "zcs/media/wire/rtp_packet.h"
#include "zcs/util/dns_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *state_name(zcs_rtsp_client_state st)
{
    switch (st) {
    case ST_IDLE:
        return "IDLE";
    case ST_CONNECTING:
        return "CONNECTING";
    case ST_OPTIONS:
        return "OPTIONS";
    case ST_DESCRIBE:
        return "DESCRIBE";
    case ST_SETUP:
        return "SETUP";
    case ST_PLAY:
        return "PLAY";
    case ST_PLAYING:
        return "PLAYING";
    case ST_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

static zn_err_t parse_rtsp_url(const char *url, zcs_rtsp_url *out)
{
    if (!url || !out) {
        return ZN_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (strncmp(p, "rtsps://", 8) == 0) {
        out->use_tls = 1;
        out->scheme = "rtsps";
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "rtsp://", 7) == 0) {
        out->scheme = "rtsp";
        out->port = ZCS_RTSP_DEFAULT_PORT;
        p += 7;
    } else {
        return ZN_ERR_INVALID;
    }

    const char *slash = strchr(p, '/');
    const char *at = strrchr(p, '@');
    if (at) {
        char cred[192];
        size_t clen = (size_t)(at - p);
        if (clen >= sizeof(cred)) {
            clen = sizeof(cred) - 1;
        }
        memcpy(cred, p, clen);
        cred[clen] = '\0';
        char *colon = strchr(cred, ':');
        if (colon) {
            *colon = '\0';
            strncpy(out->user, cred, sizeof(out->user) - 1);
            strncpy(out->pass, colon + 1, sizeof(out->pass) - 1);
        } else {
            strncpy(out->user, cred, sizeof(out->user) - 1);
        }
    }
    const char *host_start = at ? at + 1 : p;
    const char *host_end = slash ? slash : p + strlen(p);

    const char *colon = NULL;
    for (const char *c = host_start; c < host_end; ++c) {
        if (*c == ':') {
            colon = c;
            break;
        }
    }

    size_t host_len = colon ? (size_t)(colon - host_start) : (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return ZN_ERR_INVALID;
    }
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    if (colon) {
        out->port = (uint16_t)atoi(colon + 1);
        if (out->port == 0) {
            out->port = out->use_tls ? 443 : ZCS_RTSP_DEFAULT_PORT;
        }
    }

    if (slash) {
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else {
        strncpy(out->path, "/", sizeof(out->path) - 1);
    }

    {
        int n = snprintf(out->full, sizeof(out->full), "%s://%s:%u%s", out->scheme, out->host,
                         out->port, out->path);
        if (n < 0 || (size_t)n >= sizeof(out->full)) {
            return ZN_ERR_INVALID;
        }
    }
    return ZN_OK;
}

static zn_err_t client_io_connect(zcs_rtsp_client *c)
{
    char ipbuf[64];
    const char *target;

    if (!c) {
        return ZN_ERR_INVALID;
    }
    /* 主机名走缓存解析成 IP：poller 上不再每次连接都做 DNS。SNI 已在 create 时设置。 */
    target = zcs_dns_target(c->url.host, ipbuf, sizeof(ipbuf));
#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        return zn_tls_client_connect(c->tls, target, c->url.port);
    }
#endif
    if (c->tcp) {
        return zn_tcp_client_connect(c->tcp, target, c->url.port);
    }
    return ZN_ERR_STATE;
}

static int client_io_is_connected(const zcs_rtsp_client *c)
{
    if (!c) {
        return 0;
    }
#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        return zn_tls_client_is_connected(c->tls);
    }
#endif
    if (c->tcp) {
        return zn_tcp_client_is_connected(c->tcp);
    }
    return 0;
}

static void client_io_close(zcs_rtsp_client *c)
{
    if (!c) {
        return;
    }
#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        zn_tls_client_close(c->tls);
        return;
    }
#endif
    if (c->tcp) {
        zn_tcp_client_close(c->tcp);
    }
}

static void client_cancel_retry(zcs_rtsp_client *c)
{
    if (c && c->retry_timer) {
        zn_poller_timer_cancel(c->retry_timer);
        c->retry_timer = NULL;
    }
}

static void client_cancel_close(zcs_rtsp_client *c)
{
    if (c && c->close_timer) {
        zn_poller_timer_cancel(c->close_timer);
        c->close_timer = NULL;
    }
}

static void client_reset_state(zcs_rtsp_client *c)
{
    if (!c) {
        return;
    }
    /* 复位到“刚连上、还没做任何协商”的干净状态：拆掉 UDP 接收资源、清 session
     * 与凭据的“本次会话”部分（digest 缓存也清，下次 401 重新挑战）、关掉
     * splitter 的 RTP 分流、复位 RTP receiver 的乱序状态。重连与首次 play 都
     * 复用这段，保证两者起点一致。 */
    rtsp_client_udp_teardown(c);
    c->state = ST_IDLE;
    c->cseq = 1;
    c->session_id[0] = '\0';
    c->content_base[0] = '\0';
    c->setup_index = 0;
    c->teardown_sent = 0;
    c->auto_udp_tried = 0;
    c->auth_retry_done = 0;
    c->digest_realm[0] = '\0';
    c->digest_nonce[0] = '\0';
    memset(&c->session, 0, sizeof(c->session));
    zcs_rtsp_splitter_enable_rtp(c->splitter, 0);
    if (c->receiver) {
        zcs_rtp_receiver_reset(c->receiver);
    }
}

static uint64_t client_retry_task(void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    c->retry_timer = NULL;
    if (!c || c->stopping) {
        return 0;
    }
    zn_debug("RTSP client retry #%d url=%s", c->failed_count + 1, c->url.full);
    /* 重连前彻底复位协议状态（TEARDOWN UDP、清 session id/splitter/receiver），
     * 但保留 pc 重连计数与 digest 凭据——凭据在 401 后仍需复用。
     * connect 失败则返回延迟秒数让定时器自我重排，实现退避。 */
    client_reset_state(c);
    if (client_io_connect(c) != ZN_OK) {
        return (uint64_t)c->reconnect_delay_ms;
    }
    c->state = ST_CONNECTING;
    return 0;
}

void rtsp_client_fail(zcs_rtsp_client *c, zn_err_t err)
{
    if (!c || c->stopping) {
        return;
    }
    zn_warn("RTSP client fail state=%s err=%d url=%s", state_name(c->state), (int)err,
             c->url.full);
    c->state = ST_ERROR;
    if (c->opts.on_error) {
        c->opts.on_error(err, c->opts.user);
    }
    /* retry_count<0 表示无限重试；>=0 时达到上限就彻底放弃（不排重连定时器）。
     * 重连本身由 client_retry_task 复位状态并重新 connect，因此这里只负责
     * 计数与排程，不直接拆 socket——拆 socket 在 stop 路径做，避免正常重连
     * 把还在用的 fd 关掉。 */
    if (c->retry_count >= 0 && c->failed_count >= c->retry_count) {
        return;
    }
    ++c->failed_count;
    client_cancel_retry(c);
    c->retry_timer =
        zn_poller_do_delay(c->opts.poller, (uint64_t)c->reconnect_delay_ms, client_retry_task, c);
}

static zn_err_t client_send_raw(zcs_rtsp_client *c, const char *data, size_t len)
{
#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        return zn_tls_client_send(c->tls, data, len);
    }
#endif
    if (c->tcp) {
        return zn_tcp_client_send(c->tcp, data, len);
    }
    return ZN_ERR_STATE;
}

static const char *method_public_name(zcs_rtsp_method method)
{
    switch (method) {
    case ZCS_RTSP_OPTIONS:
        return "OPTIONS";
    case ZCS_RTSP_DESCRIBE:
        return "DESCRIBE";
    case ZCS_RTSP_SETUP:
        return "SETUP";
    case ZCS_RTSP_PLAY:
        return "PLAY";
    case ZCS_RTSP_PAUSE:
        return "PAUSE";
    case ZCS_RTSP_TEARDOWN:
        return "TEARDOWN";
    case ZCS_RTSP_GET_PARAMETER:
        return "GET_PARAMETER";
    case ZCS_RTSP_SET_PARAMETER:
        return "SET_PARAMETER";
    case ZCS_RTSP_ANNOUNCE:
        return "ANNOUNCE";
    case ZCS_RTSP_RECORD:
        return "RECORD";
    default:
        return "OPTIONS";
    }
}

static void client_apply_credentials(zcs_rtsp_client *c)
{
    if (!c) {
        return;
    }
    if (c->opts.username && c->opts.username[0]) {
        strncpy(c->url.user, c->opts.username, sizeof(c->url.user) - 1);
    }
    if (c->opts.password && c->opts.password[0]) {
        strncpy(c->url.pass, c->opts.password, sizeof(c->url.pass) - 1);
    }
}

static zn_err_t client_send(zcs_rtsp_client *c, zcs_rtsp_method method, const char *url,
                             const char *extra, const char *body, size_t body_len)
{
    char buf[4096];
    char auth[512];
    char merged[1024];
    size_t n = 0;

    auth[0] = '\0';
    /* 已有 digest 挑战信息时，对每个请求重新算 Authorization（含本次方法/URL
     * 与递增的 CSeq，某些服务器要求 nc 递增；这里用 CSeq 作为 nonce-count 的
     * 近似来源由 build 内部处理）。 */
    if (c->digest_realm[0] && c->digest_nonce[0] && c->url.user[0] && c->url.pass[0]) {
        (void)zcs_rtsp_digest_build_authorization(method_public_name(method), url, c->url.user,
                                                  c->url.pass, c->digest_realm, c->digest_nonce,
                                                  auth, sizeof(auth));
    }

    /* 把调用方给的 extra 头与自动生成的 Authorization 合并进同一缓冲，再交给
     * 报文构造器（它只管方法行/必需头，不管这两类可选头）。 */
    merged[0] = '\0';
    if (extra && extra[0]) {
        strncpy(merged, extra, sizeof(merged) - 1);
    }
    if (auth[0]) {
        size_t ml = strlen(merged);
        if (ml + strlen(auth) + 1 < sizeof(merged)) {
            strcat(merged + ml, auth);
        }
    }
    const char *send_extra = merged[0] ? merged : NULL;

    zn_err_t err = zcs_rtsp_message_build_request(buf, sizeof(buf), method, url, c->cseq++,
                                                   c->session_id[0] ? c->session_id : NULL,
                                                   send_extra, body, body_len, &n);
    if (err != ZN_OK) {
        return err;
    }
    return client_send_raw(c, buf, n);
}

static const char *setup_url(zcs_rtsp_client *c, const zcs_media_track *track)
{
    /* SETUP 的 URL 按 SDP 里 a=control 的三种写法分别拼：
     *   1. 以 '/' 开头：绝对路径，补上 scheme+host:port；
     *   2. 含 "://"：已是完整 URL，直接用；
     *   3. 其它：相对路径，拼到 Content-Base（或回落到会话 URL）后面。
     * 用 static 缓冲是因为返回值要活到 client_send 使用为止，且本函数只在
     * poller 单线程调用，不存在重入。 */
    static char buf[ZCS_RTSP_SETUP_URL_MAX];

    buf[0] = '\0';
    if (track->control[0] == '/') {
        snprintf(buf, sizeof(buf), "rtsp://%s:%u%s", c->url.host, c->url.port, track->control);
    } else if (strstr(track->control, "://")) {
        strncpy(buf, track->control, sizeof(buf) - 1);
    } else if (c->content_base[0]) {
        snprintf(buf, sizeof(buf), "%s%s%s", c->content_base,
                 c->content_base[strlen(c->content_base) - 1] == '/' ? "" : "/", track->control);
    } else {
        snprintf(buf, sizeof(buf), "%s%s", c->url.full, track->control);
    }
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

static void send_options(zcs_rtsp_client *c)
{
    c->state = ST_OPTIONS;
    zn_debug("RTSP client -> OPTIONS %s", c->url.full);
    client_send(c, ZCS_RTSP_OPTIONS, c->url.full, NULL, NULL, 0);
}

static void send_describe(zcs_rtsp_client *c)
{
    c->state = ST_DESCRIBE;
    zn_debug("RTSP client -> DESCRIBE %s", c->url.full);
    const char *extra = "Accept: application/sdp\r\n";
    client_send(c, ZCS_RTSP_DESCRIBE, c->url.full, extra, NULL, 0);
}

static void send_setup(zcs_rtsp_client *c)
{
    if (c->setup_index >= c->session.track_count) {
        /* 所有轨都 SETUP 完毕 → 发 PLAY。Range: npt=0 表示从直播最旧处起播。 */
        c->state = ST_PLAY;
        zn_debug("RTSP client -> PLAY %s", c->url.full);
        client_send(c, ZCS_RTSP_PLAY, c->url.full, "Range: npt=0.000-\r\n", NULL, 0);
        return;
    }

    c->state = ST_SETUP;
    const zcs_media_track *track = &c->session.tracks[c->setup_index];
    char extra[256];

    if (c->rtp_mode == ZCS_RTSP_RTP_UDP) {
        /* UDP：先为本轨分配本地端口并算出 Transport 头（client_port=...）。 */
        if (rtsp_client_udp_prepare_track(c, c->setup_index) != ZN_OK) {
            rtsp_client_fail(c, ZN_ERR_IO);
            return;
        }
        if (rtsp_client_udp_build_setup_extra(c, c->setup_index, extra, sizeof(extra)) < 0) {
            rtsp_client_fail(c, ZN_ERR_IO);
            return;
        }
    } else {
        /* TCP：RTP 走 RTSP 连接内的 interleaved 通道，用 SDP 预分配的两个
         * channel 号（rtp/rtcp 相邻）作为通道标识，后续从同一 socket 里按
         * 4 字节 interleaved 头识别。 */
        snprintf(extra, sizeof(extra), "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
                 track->interleaved_rtp, track->interleaved_rtcp);
    }

    zn_debug("RTSP client -> SETUP track=%u mode=%s", c->setup_index,
              c->rtp_mode == ZCS_RTSP_RTP_UDP ? "udp" : "tcp");
    client_send(c, ZCS_RTSP_SETUP, setup_url(c, track), extra, NULL, 0);
}


static int rtsp_client_zm_on_packet(void *user, const zm_packet *pkt)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    zcs_frame frame;

    if (!c || !c->opts.on_frame || !pkt) {
        return 0;
    }
    if (zcs_zm_frame_from_packet(pkt, &frame) == ZM_OK) {
        c->opts.on_frame(&frame, c->opts.user);
    }
    return 0;
}

static int rtsp_client_zm_ensure_track(zcs_rtsp_client *c, int track_index)
{
    const zcs_media_track *t;
    int clock_hz;

    if (!c || !c->zm_rtp || track_index < 0 ||
        (unsigned)track_index >= c->session.track_count) {
        return -1;
    }
    t = &c->session.tracks[track_index];
    if (t->codec == ZCS_CODEC_INVALID) {
        return -1;
    }
    clock_hz = t->sample_rate > 0 ? t->sample_rate : (t->type == ZCS_TRACK_VIDEO ? 90000 : 44100);
    return zcs_zm_rtp_ingest_track((zcs_zm_rtp_ingest *)c->zm_rtp, track_index, t->codec,
                                    t->payload_type, clock_hz, NULL);
}

void rtsp_client_emit_rtp_frame(zcs_rtsp_client *c, int track_index, const zcs_rtp_packet *pkt)
{
    const zcs_media_track *track;
    static int logged_track[ZCS_SDP_TRACK_MAX];

    if (!c || !c->opts.on_frame || !pkt || track_index < 0 ||
        (unsigned)track_index >= c->session.track_count) {
        return;
    }

    track = &c->session.tracks[track_index];
    if (track->codec == ZCS_CODEC_INVALID) {
        return;
    }

    if (!logged_track[track_index]) {
        logged_track[track_index] = 1;
        zn_debug("RTSP client first RTP via zm: track=%d codec=%s pt=%u", track_index,
                  zcs_codec_name(track->codec), (unsigned)pkt->hdr.pt);
    }

    /* 惰性创建 zm RTP ingest：只有真正来 RTP 时才分配，避免空会话占用。
     * zm ingest 负责 depacketize（RTP→ES）并回调 on_frame；它内部按 track
     * 维护去包/重排状态。 */
    if (!c->zm_rtp) {
        c->zm_rtp = zcs_zm_rtp_ingest_create(ZCS_SDP_TRACK_MAX, 0, rtsp_client_zm_on_packet, c);
    }
    if (c->zm_rtp && rtsp_client_zm_ensure_track(c, track_index) == 0 && pkt->data &&
        pkt->size > 0) {
        (void)zcs_zm_rtp_ingest_input((zcs_zm_rtp_ingest *)c->zm_rtp, track_index, pkt->data,
                                       pkt->size);
    }
}

static void on_sorted_rtp(const zcs_rtp_packet *pkt, int track_index, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    if (!c || track_index < 0 || (unsigned)track_index >= c->session.track_count) {
        return;
    }
    rtsp_client_emit_rtp_frame(c, track_index, pkt);
}

static void on_rtp_payload(uint8_t channel, const uint8_t *data, size_t len, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    /* 只处理 TCP interleaved 模式：UDP 模式下 RTP 走独立 socket，不经过
     * splitter 的这个回调。 */
    if (!c || !c->receiver || c->rtp_mode != ZCS_RTSP_RTP_TCP) {
        return;
    }

    /* interleaved 通道里混着 RTCP（SR/RR/PLI 等），这里只喂 RTP 给 receiver，
     * RTCP 直接丢弃——拉流客户端不需要处理对端控制报文。 */
    if (zcs_rtp_is_rtcp(data, len)) {
        return;
    }

    /* 通道号 → track_index：SETUP 响应的 interleaved 编号可能被服务器改写，
     * 所以按实际记录的值反查，而不是假定 SDP 里的原值。 */
    int track_index = -1;
    for (unsigned i = 0; i < c->session.track_count; ++i) {
        if (c->session.tracks[i].interleaved_rtp == channel) {
            track_index = (int)i;
            break;
        }
    }
    if (track_index < 0) {
        return;
    }
    zcs_rtp_receiver_input(c->receiver, track_index, data, len);
}

static void retry_current_request(zcs_rtsp_client *c)
{
    if (!c) {
        return;
    }
    switch (c->state) {
    case ST_OPTIONS:
        send_options(c);
        break;
    case ST_DESCRIBE:
        send_describe(c);
        break;
    case ST_SETUP:
        send_setup(c);
        break;
    case ST_PLAY:
        client_send(c, ZCS_RTSP_PLAY, c->url.full, "Range: npt=0.000-\r\n", NULL, 0);
        break;
    default:
        break;
    }
}

static int handle_auth_401(zcs_rtsp_client *c, const zcs_rtsp_message *msg)
{
    /* 只在“首次且带用户名”时重试：auth_retry_done 防死循环（若重试后仍
     * 401 就判失败）。解析出 Digest realm/nonce 后，重发“当前状态对应的
     * 那个请求”（retry_current_request），凭据会被 client_send 自动附加
     * 到后续每个请求上。 */
    if (!c || c->auth_retry_done || !c->url.user[0]) {
        return 0;
    }
    const char *www = zcs_rtsp_message_get(msg, "WWW-Authenticate");
    char realm[128], nonce[128];
    zcs_rtsp_auth_scheme scheme =
        zcs_rtsp_digest_parse_www_auth(www, realm, sizeof(realm), nonce, sizeof(nonce));
    if (scheme != ZCS_RTSP_AUTH_DIGEST || !realm[0] || !nonce[0]) {
        return 0;
    }
    strncpy(c->digest_realm, realm, sizeof(c->digest_realm) - 1);
    strncpy(c->digest_nonce, nonce, sizeof(c->digest_nonce) - 1);
    c->auth_retry_done = 1;
    zn_debug("RTSP client 401 -> retry with Digest user=%s", c->url.user);
    retry_current_request(c);
    return 1;
}

static void rtsp_client_auto_fallback_udp(zcs_rtsp_client *c)
{
    if (!c || c->auto_udp_tried) {
        return;
    }
    /* AUTO 模式下先试 TCP，服务器回 461（Unsupported Transport）说明它不吃
     * interleaved，就一次性回落到 UDP 并从头 SETUP。auto_udp_tried 保证只
     * 回落一次，避免两种模式来回抖。 */
    zn_debug("RTSP client AUTO: TCP SETUP failed (461), fallback to UDP");
    c->auto_udp_tried = 1;
    c->rtp_mode = ZCS_RTSP_RTP_UDP;
    c->setup_index = 0;
    rtsp_client_udp_teardown(c);
    send_setup(c);
}

static void handle_response(zcs_rtsp_client *c, const zcs_rtsp_message *msg)
{
    if (msg->status_code == 401) {
        if (handle_auth_401(c, msg)) {
            return;
        }
        zn_warn("RTSP client auth failed status=401 state=%s", state_name(c->state));
        rtsp_client_fail(c, ZN_ERR_IO);
        return;
    }
    /* 461 = Unsupported Transport：仅在 AUTO 且当前是 TCP SETUP 时回落到 UDP。 */
    if (msg->status_code == 461 && c->requested_rtp_mode == ZCS_RTSP_RTP_AUTO &&
        c->rtp_mode == ZCS_RTSP_RTP_TCP && !c->auto_udp_tried && c->state == ST_SETUP) {
        rtsp_client_auto_fallback_udp(c);
        return;
    }
    if (msg->status_code != 200) {
        zn_warn("RTSP client %s failed status=%d", state_name(c->state), msg->status_code);
        rtsp_client_fail(c, ZN_ERR_IO);
        return;
    }

    /* Session 头可能带 ";timeout=..." 参数，去掉分号后缀只留 session id。 */
    const char *sid = zcs_rtsp_message_get(msg, "Session");
    if (sid && sid[0]) {
        strncpy(c->session_id, sid, sizeof(c->session_id) - 1);
        char *semi = strchr(c->session_id, ';');
        if (semi) {
            *semi = '\0';
        }
    }

    const char *base = zcs_rtsp_message_get(msg, "Content-Base");
    if (base && base[0]) {
        strncpy(c->content_base, base, sizeof(c->content_base) - 1);
    }

    switch (c->state) {
    case ST_OPTIONS:
        /* OPTIONS 只是探测支持的方法，无论结果如何都继续 DESCRIBE。 */
        send_describe(c);
        break;
    case ST_DESCRIBE:
        if (zcs_rtsp_message_body(msg) && zcs_rtsp_message_body_len(msg) > 0) {
            if (zcs_sdp_parse(zcs_rtsp_message_body(msg), zcs_rtsp_message_body_len(msg), &c->session) != ZN_OK) {
                rtsp_client_fail(c, ZN_ERR_INVALID);
                return;
            }
            zn_debug("RTSP client DESCRIBE ok tracks=%u transport=%s", c->session.track_count,
                      c->rtp_mode == ZCS_RTSP_RTP_UDP ? "udp" : "tcp");
            /* 先通知上层每条轨（建源、拿 codec/fmtp），再开始逐轨 SETUP。 */
            for (unsigned i = 0; i < c->session.track_count; ++i) {
                if (c->opts.on_track) {
                    c->opts.on_track(&c->session.tracks[i], c->opts.user);
                }
            }
        }
        c->setup_index = 0;
        send_setup(c);
        break;
    case ST_SETUP: {
        const char *tr = zcs_rtsp_message_get(msg, "Transport");
        if (tr && c->setup_index < c->session.track_count) {
            if (c->rtp_mode == ZCS_RTSP_RTP_UDP) {
                rtsp_client_udp_apply_setup_response(c, c->setup_index, tr);
            } else {
                /* TCP：以服务器回的 interleaved 编号为准（可能与请求不同），
                 * 记录后供 on_rtp_payload 反查 track。 */
                uint8_t ich = 0, icc = 0;
                if (zcs_rtsp_transport_parse_interleaved(tr, &ich, &icc) == 0) {
                    c->session.tracks[c->setup_index].interleaved_rtp = ich;
                    c->session.tracks[c->setup_index].interleaved_rtcp = icc;
                    zn_debug("RTSP client SETUP ok track=%u interleaved=%u-%u", c->setup_index,
                              (unsigned)ich, (unsigned)icc);
                }
            }
        }
        c->setup_index++;
        send_setup(c);
        break;
    }
    case ST_PLAY:
        /* PLAY 成功 → 进入真正收 RTP 的阶段。TCP 要打开 splitter 的 RTP 分流，
         * UDP 要启动接收侧（收 socket 已在 SETUP 时绑定）。同时复位失败计数、
         * 取消重连定时器，表示本次连接已稳定。 */
        c->state = ST_PLAYING;
        c->failed_count = 0;
        client_cancel_retry(c);
        if (c->rtp_mode == ZCS_RTSP_RTP_TCP) {
            zcs_rtsp_splitter_enable_rtp(c->splitter, 1);
        } else {
            rtsp_client_udp_on_play(c);
        }
        zn_debug("RTSP client PLAYING: tracks=%u url=%s", c->session.track_count, c->url.full);
        if (c->opts.on_ready) {
            c->opts.on_ready(c->opts.user);
        }
        break;
    default:
        break;
    }
}

static void on_rtsp_message(const zcs_rtsp_message *msg, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    if (!c || c->stopping) {
        return;
    }
    if (zcs_rtsp_message_is_response(msg)) {
        handle_response(c, msg);
    }
}

static void on_transport_connected(zcs_rtsp_client *c)
{
    zn_debug("RTSP client connected %s:%u tls=%d", c->url.host, (unsigned)c->url.port,
              c->url.use_tls);
    send_options(c);
}

static void on_transport_recv(zcs_rtsp_client *c, const void *data, size_t len)
{
    if (!c || c->stopping) {
        return;
    }
    /* 所有入站字节先喂 splitter：它负责把 RTSP 消息、interleaved RTP/RTCP 从
     * 同一条流里切开，再分别回调 on_rtsp_message / on_rtp_payload。 */
    zcs_rtsp_splitter_input(c->splitter, data, len);
}

static void on_tcp_connect(zn_tcp_client *client, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    (void)client;
    on_transport_connected(c);
}

static void on_tcp_recv(zn_tcp_client *client, const void *data, size_t len, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    (void)client;
    on_transport_recv(c, data, len);
}

static void on_tcp_error(zn_tcp_client *client, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    (void)client;
    rtsp_client_fail(c, ZN_ERR_IO);
}

static void on_tls_connect(zn_tls_client *client, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    (void)client;
    on_transport_connected(c);
}

static void on_tls_recv(zn_tls_client *client, const void *data, size_t len, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    (void)client;
    on_transport_recv(c, data, len);
}

static void on_tls_error(zn_tls_client *client, void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    (void)client;
    rtsp_client_fail(c, ZN_ERR_IO);
}

static uint64_t client_close_task(void *user)
{
    zcs_rtsp_client *c = (zcs_rtsp_client *)user;
    c->close_timer = NULL;
    if (c) {
        client_io_close(c);
    }
    return 0;
}

zcs_rtsp_client *zcs_rtsp_client_create(const zcs_rtsp_client_opts *opts)
{
    if (!opts || !opts->poller || !opts->url) {
        return NULL;
    }

    zcs_rtsp_client *c = (zcs_rtsp_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opts = *opts;
    c->cseq = 1;
    /* requested_rtp_mode 记录用户的原始选择；rtp_mode 是“当前执行的模式”。
     * AUTO 先落 TCP（多数服务器支持 interleaved，且对 NAT 友好），461 时再
     * 回落 UDP，见 rtsp_client_auto_fallback_udp。 */
    c->requested_rtp_mode = opts->rtp_mode;
    c->rtp_mode = (opts->rtp_mode == ZCS_RTSP_RTP_UDP) ? ZCS_RTSP_RTP_UDP : ZCS_RTSP_RTP_TCP;
    /* 约定：opts->retry_count==0 表示“调用方没设”，归一为 -1=无限重试；
     * 否则按其给定次数。reconnect_delay_ms 缺省 2s。 */
    c->retry_count = opts->retry_count;
    if (c->retry_count == 0) {
        c->retry_count = -1;
    }
    c->reconnect_delay_ms = opts->reconnect_delay_ms > 0 ? opts->reconnect_delay_ms : 2000;

    if (parse_rtsp_url(opts->url, &c->url) != ZN_OK) {
        free(c);
        return NULL;
    }
    client_apply_credentials(c);

    zcs_rtsp_splitter_opts sopts = {on_rtsp_message, on_rtp_payload, c};
    c->splitter = zcs_rtsp_splitter_create(&sopts);
    if (!c->splitter) {
        free(c);
        return NULL;
    }

    zcs_rtp_receiver_opts ropts = {ZCS_SDP_TRACK_MAX, ZCS_RTP_JITTER_SLOTS_DEFAULT, on_sorted_rtp,
                                   c};
    c->receiver = zcs_rtp_receiver_create(&ropts);

    zcs_modules_register_all();

    if (c->url.use_tls) {
#if !defined(ZN_HAVE_OPENSSL) || !ZN_HAVE_OPENSSL
        zcs_rtsp_client_destroy(c);
        return NULL;
#else
        if (!opts->ssl_ctx) {
            zcs_rtsp_client_destroy(c);
            return NULL;
        }
        zn_tls_client_ops_t tops = {on_tls_connect, on_tls_recv, on_tls_error};
        zn_tls_client_opts_t topts = {opts->poller, opts->ssl_ctx, &tops, c, c->url.host};
        c->tls = zn_tls_client_create(&topts);
        if (!c->tls) {
            zcs_rtsp_client_destroy(c);
            return NULL;
        }
#endif
    } else {
        zn_tcp_client_ops_t tops = {on_tcp_connect, on_tcp_recv, on_tcp_error};
        zn_tcp_client_opts_t topts = {opts->poller, &tops, c};
        c->tcp = zn_tcp_client_create(&topts);
        if (!c->tcp) {
            zcs_rtsp_client_destroy(c);
            return NULL;
        }
    }
    if (!c->receiver) {
        zcs_rtsp_client_destroy(c);
        return NULL;
    }
    zn_debug("RTSP client create url=%s mode=%s user=%s", c->url.full,
              c->requested_rtp_mode == ZCS_RTSP_RTP_AUTO
                  ? "auto"
                  : (c->rtp_mode == ZCS_RTSP_RTP_UDP ? "udp" : "tcp"),
              c->url.user[0] ? c->url.user : "-");
    return c;
}

void zcs_rtsp_client_destroy(zcs_rtsp_client *c)
{
    if (!c) {
        return;
    }
    client_cancel_retry(c);
    client_cancel_close(c);
    zcs_rtsp_client_stop(c);
    zn_tcp_client_destroy(c->tcp);
#if ZCS_HAVE_PULL_TLS
    zn_tls_client_destroy(c->tls);
#endif
    zcs_rtsp_splitter_destroy(c->splitter);
    zcs_rtp_receiver_destroy(c->receiver);
    if (c->zm_rtp) {
        zcs_zm_rtp_ingest_destroy((zcs_zm_rtp_ingest *)c->zm_rtp);
        c->zm_rtp = NULL;
    }
    free(c);
}

zn_err_t zcs_rtsp_client_play(zcs_rtsp_client *c)
{
    if (!c) {
        return ZN_ERR_INVALID;
    }
    c->stopping = 0;
    client_reset_state(c);
    c->state = ST_CONNECTING;
    return client_io_connect(c);
}

void zcs_rtsp_client_stop(zcs_rtsp_client *c)
{
    if (!c || c->stopping) {
        return;
    }
    c->stopping = 1;
    client_cancel_retry(c);
    /* 尽量优雅地发 TEARDOWN，让服务器释放会话；前提是连接仍在且尚未发过。
     * teardown_sent 防重复。 */
    if (c->session_id[0] && client_io_is_connected(c) && !c->teardown_sent) {
        c->teardown_sent = 1;
        client_send(c, ZCS_RTSP_TEARDOWN, c->url.full, NULL, NULL, 0);
    }
    rtsp_client_udp_teardown(c);
    client_cancel_close(c);
    /* 不立即 close：TEARDOWN 刚写进发送缓冲，需要一点时间真正落 socket。
     * 延迟 80ms 再关，给对端一个收包窗口，也避免在本回调栈里同步销毁 fd。 */
    c->close_timer = zn_poller_do_delay(c->opts.poller, 80, client_close_task, c);
}

const zcs_sdp_session *zcs_rtsp_client_session(const zcs_rtsp_client *c)
{
    return c ? &c->session : NULL;
}
