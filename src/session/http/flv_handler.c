/**
 * @file flv_handler.c
 * @brief 直播 HTTP-FLV/TS 输出与 flush。
 */
#include "zcs/play/http_port.h"
#include "zcs/play/http_flv/live_muxer.h"
#include "session/http/session_internal.h"
#include "zcs/egress/mpegts/mpegts_egress.h"
#include "zcs/play/play_binding.h"
#include "zn/platform.h"
#include "zcs/core/codec_filter.h"
#include "zcs/play/dispatch.h"
#include "zcs/core/event.h"
#include "zcs/protocol/http/websocket/framer.h"
#include "zcs/protocol/http/websocket/handshake.h"
#include "zn/net/tcp_server.h"
#include "zn/util/buf.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <string.h>
#define ZCS_HTTP_TS_SEND_PAYLOAD 1316
static int http_stream_send_payload(zcs_http_session *hs, const void *data, size_t len)
{
    if (!hs || !hs->tcp || !data || len == 0) {
        return -1;
    }
    if (hs->ws_mode) {
        uint8_t whdr[10];
        size_t whlen = zcs_ws_framer_server_binary_hdr(whdr, sizeof(whdr), len);
        if (whlen == 0 || zn_tcp_session_send(hs->tcp, whdr, whlen) != ZN_OK) {
            return -1;
        }
    }
    return zn_tcp_session_send(hs->tcp, data, len) == ZN_OK ? 0 : -1;
}

/* 所有 muxer 走同一份取数 + 扩容 + 发送逻辑（见 zcs/play/sink.h）。 */
static int http_flv_send_mux_chunk(zcs_http_session *hs, zcs_sink_next_fn mux_next, void *muxer)
{
    zn_buf *buf = NULL;
    size_t n = 0;
    int r;

    if (!hs || !hs->tcp || !mux_next || !muxer) {
        return 0;
    }
    r = zcs_sink_pull(mux_next, muxer, zn_tcp_session_poller(hs->tcp), hs->send_cap,
                      ZCS_HTTP_SEND_BUF_MAX, &buf, &n);
    if (r <= 0) {
        return r;
    }
    if (http_stream_send_payload(hs, zn_buf_data(buf), n) != 0) {
        zn_buf_unref(buf);
        return -1;
    }
    zn_buf_unref(buf);
    return r;
}

static int http_ts_send_mux_chunk(zcs_http_session *hs, zcs_mpegts_egress *muxer)
{
    uint8_t buf[ZCS_HTTP_TS_SEND_PAYLOAD];
    size_t n = 0;
    int r;
    if (!hs || !hs->tcp || !muxer) {
        return 0;
    }
    r = zcs_mpegts_egress_next(muxer, buf, sizeof(buf), &n);
    if (r <= 0 || n == 0) {
        return r;
    }
    if (http_stream_send_payload(hs, buf, n) != 0) {
        return -1;
    }
    return r;
}

static int http_flv_mux_next(void *muxer, uint8_t *out, size_t cap, size_t *out_len)
{
    return zcs_flv_live_muxer_next((zcs_flv_live_muxer *)muxer, out, cap, out_len);
}

static zn_err_t http_flv_live_start_hdr(void *muxer, int has_audio, int has_video, uint8_t *out,
                                         size_t cap, size_t *out_len)
{
    return zcs_flv_live_muxer_start((zcs_flv_live_muxer *)muxer, has_audio, has_video, out, cap,
                                    out_len);
}

void zcs_http_flv_emit_header_and_stream(zcs_http_session *hs, int has_audio, int has_video,
                                         zcs_flv_start_hdr_fn start, void *muxer)
{
    zn_poller *pol;
    zn_buf *hdr;
    size_t flv_len = 0;
    if (!hs || !hs->tcp || !start || !muxer) {
        return;
    }
    pol = zn_tcp_session_poller(hs->tcp);
    hdr = zn_buf_alloc_local(pol, hs->send_cap);
    if (hdr &&
        start(muxer, has_audio, has_video, (uint8_t *)zn_buf_data(hdr), zn_buf_cap(hdr),
              &flv_len) == ZN_OK &&
        flv_len > 0) {
        zn_buf_set_len(hdr, flv_len);
        (void)http_stream_send_payload(hs, zn_buf_data(hdr), flv_len);
        zn_buf_unref(hdr);
    } else if (hdr) {
        zn_buf_unref(hdr);
    }
    zn_tcp_session_flush(hs->tcp);
    hs->state =
        hs->ws_mode ? ZCS_HTTP_SESSION_STATE_WS_STREAMING : ZCS_HTTP_SESSION_STATE_STREAMING;
    zcs_http_session_stream_flush(hs);
}

static int http_flv_vod_mux_next(void *muxer, uint8_t *out, size_t cap, size_t *out_len)
{
    return zcs_vod_flv_muxer_next((zcs_vod_flv_muxer *)muxer, out, cap, out_len);
}

void zcs_http_session_stream_flush(zcs_http_session *hs)
{
    if (!hs || (hs->state != ZCS_HTTP_SESSION_STATE_STREAMING &&
                hs->state != ZCS_HTTP_SESSION_STATE_WS_STREAMING)) {
        return;
    }
    if (!hs->live_muxer && !hs->vod_muxer && !hs->ts_muxer) {
        return;
    }
    if (hs->vod_lane) {
        zcs_play_binding bind;

        memset(&bind, 0, sizeof(bind));
        bind.vod_lane = &hs->vod_lane;
        zcs_play_binding_demux_fill(&bind, 32);
    }
    for (int i = 0; i < 24; ++i) {
        int r;
        if (hs->tcp && zn_tcp_session_out_pending(hs->tcp) > 128 * 1024) {
            break;
        }
        if (hs->tcp) {
            zn_tcp_session_flush(hs->tcp);
        }
        if (hs->ts_muxer) {
            r = http_ts_send_mux_chunk(hs, hs->ts_muxer);
        } else if (hs->live_muxer) {
            r = http_flv_send_mux_chunk(hs, http_flv_mux_next, hs->live_muxer);
        } else {
            r = http_flv_send_mux_chunk(hs, http_flv_vod_mux_next, hs->vod_muxer);
        }
        if (r <= 0) {
            break;
        }
    }
    if (hs->tcp) {
        zn_tcp_session_flush(hs->tcp);
    }
}

void zcs_http_live_flv_start(zcs_http_session *hs, zcs_media_source *src, const char *app,
                             const char *stream, const char *ws_key)
{
    char hdr[512];
    int n;
    const char *player = ws_key && ws_key[0] ? "ws-flv" : "http-flv";
    if (!hs || !src || !src->gop_queue) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (zcs_session_capability_check_source(ZCS_PROTO_CAP_HTTP_FLV_PLAY, src) != ZN_OK) {
        zcs_session_capability_log_reject(player, src, ZCS_PROTO_CAP_HTTP_FLV_PLAY);
        zcs_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }
    {

        if (!zcs_http_session_authorize_play(hs, player, src)) {
            zcs_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    hs->live_muxer = zcs_flv_live_muxer_create(src);
    if (!hs->live_muxer) {
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    if (hs->tcp) {
        zcs_flv_live_muxer_bind_poller(hs->live_muxer, zn_tcp_session_poller(hs->tcp));
    }
    zcs_flv_live_muxer_bind_source(hs->live_muxer, src);
    hs->source = src;
    zn_debug("%s live play: app=%s stream=%s video=%d audio=%d", player, app, stream,
              src->has_video, src->has_audio);
    zcs_media_source_reader_add(src);
    hs->play_start_ms = zn_monotonic_ms();
    zcs_media_event_play(src, player);
    hs->reader_attached = 1;
    hs->play_event = player;
    if (ws_key && ws_key[0]) {
        char accept[32];
        if (zcs_ws_handshake_accept(ws_key, accept, sizeof(accept)) != 0) {
            zcs_http_response_send_error(hs, 400, "Bad Request");
            return;
        }
        n = (int)zcs_ws_handshake_response(hdr, sizeof(hdr), accept);
        if (n <= 0) {
            zcs_http_response_send_error(hs, 500, "Internal Server Error");
            return;
        }
        hs->ws_mode = 1;
    } else {
        n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 200 OK\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Type: video/x-flv\r\n"
                     "Access-Control-Allow-Origin: *\r\n\r\n");
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);
    zcs_http_flv_emit_header_and_stream(hs, src->has_audio, src->has_video, http_flv_live_start_hdr,
                                        hs->live_muxer);
}

void zcs_http_live_ts_start(zcs_http_session *hs, zcs_media_source *src, const char *app,
                            const char *stream)
{
    char hdr[384];
    int n;
    zcs_session_play_opts pcfg;
    if (!hs || !src || !src->gop_queue) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (zcs_session_capability_check_source(ZCS_PROTO_CAP_HTTP_TS_PLAY, src) != ZN_OK) {
        zcs_session_capability_log_reject("http-ts", src, ZCS_PROTO_CAP_HTTP_TS_PLAY);
        zcs_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }
    {

        if (!zcs_http_session_authorize_play(hs, "http-ts", src)) {
            zcs_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    zcs_session_dispatch_register_all();
    zcs_egress_source_init(&hs->play);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZCS_SESSION_HTTP_TS;
    if (zcs_session_attach_play(ZCS_SESSION_HTTP_TS, &hs->play, src, &pcfg) != ZN_OK) {
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    hs->ts_muxer = zcs_mpegts_egress_create(src, &hs->play);
    if (!hs->ts_muxer) {
        zcs_session_detach_play(ZCS_SESSION_HTTP_TS, &hs->play);
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    hs->source = src;
    zn_debug("HTTP-TS live play: app=%s stream=%s video=%d audio=%d", app, stream, src->has_video,
              src->has_audio);
    zcs_media_source_reader_add(src);
    hs->play_start_ms = zn_monotonic_ms();
    zcs_media_event_play(src, "http-ts");
    hs->reader_attached = 1;
    hs->play_event = "http-ts";
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Connection: keep-alive\r\n"
                 "Content-Type: video/mp2t\r\n"
                 "Access-Control-Allow-Origin: *\r\n\r\n");
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);
    hs->state = ZCS_HTTP_SESSION_STATE_STREAMING;
    zcs_http_session_stream_flush(hs);
}
