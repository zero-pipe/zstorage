/**
 * @file player.c
 * @brief 远端拉流播放器（组合各协议客户端）。
 */
#include "zcs/pull/player.h"
#include "zcs/core/module_registry.h"
#include "zcs/protocol/http/hls_client.h"
#include "zcs/protocol/http/flv_client.h"
#include "zcs/protocol/rtsp/client.h"
#include <stdlib.h>
#include <string.h>

typedef enum zcs_player_backend {
    PLAYER_BACKEND_INVALID = 0,
    PLAYER_BACKEND_RTSP,
    PLAYER_BACKEND_HTTP_FLV,
    PLAYER_BACKEND_HLS,
    PLAYER_BACKEND_RTMP_UNSUPPORTED,
} zcs_player_backend;

struct zcs_player {
    zcs_player_opts opts;
    zcs_player_backend backend;
    union {
        zcs_rtsp_client *rtsp;
        zcs_http_flv_client *http_flv;
        zcs_http_hls_client *hls;
        void *ptr;
    } u;
};

static int starts_with(const char *s, const char *prefix)
{
    size_t n;

    if (!s || !prefix) {
        return 0;
    }
    n = strlen(prefix);
    return strncmp(s, prefix, n) == 0;
}

static int contains_token(const char *s, const char *token)
{
    return s && token && strstr(s, token) != NULL;
}

static zcs_player_backend detect_backend(const char *url)
{
    if (starts_with(url, "rtsp://") || starts_with(url, "rtsps://")) {
        return PLAYER_BACKEND_RTSP;
    }
    if (starts_with(url, "rtmp://") || starts_with(url, "rtmps://")) {
        return PLAYER_BACKEND_RTMP_UNSUPPORTED;
    }
    if (starts_with(url, "http://") || starts_with(url, "https://")) {
        if (contains_token(url, ".m3u8")) {
            return PLAYER_BACKEND_HLS;
        }
        return PLAYER_BACKEND_HTTP_FLV;
    }
    return PLAYER_BACKEND_INVALID;
}

static void player_on_ready(void *user)
{
    zcs_player *p = (zcs_player *)user;
    if (p && p->opts.on_ready) {
        p->opts.on_ready(p->opts.user);
    }
}

static void player_on_track(const zcs_media_track *track, void *user)
{
    zcs_player *p = (zcs_player *)user;
    if (p && p->opts.on_track) {
        p->opts.on_track(track, p->opts.user);
    }
}

static void player_on_frame(const zcs_frame *frame, void *user)
{
    zcs_player *p = (zcs_player *)user;
    if (p && p->opts.on_frame) {
        p->opts.on_frame(frame, p->opts.user);
    }
}

static void player_on_error(zn_err_t err, void *user)
{
    zcs_player *p = (zcs_player *)user;
    if (p && p->opts.on_error) {
        p->opts.on_error(err, p->opts.user);
    }
}

static int player_create_rtsp(zcs_player *p)
{
    zcs_rtsp_client_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.poller = p->opts.poller;
    opts.url = p->opts.url;
    opts.ssl_ctx = p->opts.ssl_ctx;
    opts.rtp_mode = p->opts.rtsp_rtp_mode;
    opts.retry_count = p->opts.retry_count;
    opts.reconnect_delay_ms = p->opts.reconnect_delay_ms;
    opts.on_ready = player_on_ready;
    opts.on_track = player_on_track;
    opts.on_frame = player_on_frame;
    opts.on_error = player_on_error;
    opts.user = p;
    p->u.rtsp = zcs_rtsp_client_create(&opts);
    return p->u.rtsp != NULL;
}

static int player_create_http_flv(zcs_player *p)
{
    zcs_http_flv_client_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.poller = p->opts.poller;
    opts.url = p->opts.url;
    opts.ssl_ctx = p->opts.ssl_ctx;
    opts.on_ready = player_on_ready;
    opts.on_frame = player_on_frame;
    opts.on_error = player_on_error;
    opts.user = p;
    p->u.http_flv = zcs_http_flv_client_create(&opts);
    return p->u.http_flv != NULL;
}

static int player_create_hls(zcs_player *p)
{
    zcs_http_hls_client_opts opts;

    memset(&opts, 0, sizeof(opts));
    opts.poller = p->opts.poller;
    opts.url = p->opts.url;
    opts.ssl_ctx = p->opts.ssl_ctx;
    opts.on_ready = player_on_ready;
    opts.on_frame = player_on_frame;
    opts.on_error = player_on_error;
    opts.user = p;
    p->u.hls = zcs_http_hls_client_create(&opts);
    return p->u.hls != NULL;
}

zcs_player *zcs_player_create(const zcs_player_opts *opts)
{
    zcs_player *p;

    if (!opts || !opts->poller || !opts->url) {
        return NULL;
    }
    p = (zcs_player *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->opts = *opts;
    p->backend = detect_backend(opts->url);
    zcs_modules_register_all();

    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        if (!player_create_rtsp(p)) {
            goto fail;
        }
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        if (!player_create_http_flv(p)) {
            goto fail;
        }
        break;
    case PLAYER_BACKEND_HLS:
        if (!player_create_hls(p)) {
            goto fail;
        }
        break;
    case PLAYER_BACKEND_RTMP_UNSUPPORTED:
        break;
    default:
        goto fail;
    }
    return p;

fail:
    zcs_player_destroy(p);
    return NULL;
}

void zcs_player_destroy(zcs_player *p)
{
    if (!p) {
        return;
    }
    zcs_player_stop(p);
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        zcs_rtsp_client_destroy(p->u.rtsp);
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        zcs_http_flv_client_destroy(p->u.http_flv);
        break;
    case PLAYER_BACKEND_HLS:
        zcs_http_hls_client_destroy(p->u.hls);
        break;
    default:
        break;
    }
    free(p);
}

zn_err_t zcs_player_play(zcs_player *p)
{
    zn_err_t err = ZN_ERR_INVALID;

    if (!p) {
        return ZN_ERR_INVALID;
    }
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        err = zcs_rtsp_client_play(p->u.rtsp);
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        err = zcs_http_flv_client_play(p->u.http_flv);
        break;
    case PLAYER_BACKEND_HLS:
        err = zcs_http_hls_client_play(p->u.hls);
        break;
    case PLAYER_BACKEND_RTMP_UNSUPPORTED:
        err = ZN_ERR_NOT_IMPL;
        break;
    default:
        err = ZN_ERR_INVALID;
        break;
    }
    if (err != ZN_OK && p->opts.on_error) {
        p->opts.on_error(err, p->opts.user);
    }
    return err;
}

void zcs_player_stop(zcs_player *p)
{
    if (!p) {
        return;
    }
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        zcs_rtsp_client_stop(p->u.rtsp);
        break;
    case PLAYER_BACKEND_HTTP_FLV:
        zcs_http_flv_client_stop(p->u.http_flv);
        break;
    case PLAYER_BACKEND_HLS:
        zcs_http_hls_client_stop(p->u.hls);
        break;
    default:
        break;
    }
}

const char *zcs_player_protocol(const zcs_player *p)
{
    if (!p) {
        return NULL;
    }
    switch (p->backend) {
    case PLAYER_BACKEND_RTSP:
        return "rtsp";
    case PLAYER_BACKEND_HTTP_FLV:
        return "http-flv";
    case PLAYER_BACKEND_HLS:
        return "hls";
    case PLAYER_BACKEND_RTMP_UNSUPPORTED:
        return "rtmp";
    default:
        return NULL;
    }
}
