/**
 * @file dispatch.c
 * @brief 播放分发注册表：把各协议 register 接到统一 dispatch。
 */
#include "zcs/play/dispatch.h"
#include "zcs/core/codec_filter.h"
#include "zcs/vod/io/source.h"
#include <string.h>

#define ZCS_SESSION_DISPATCH_MAX 12

static const zcs_session_dispatch_ops *g_dispatch[ZCS_SESSION_DISPATCH_MAX];
static int g_dispatch_count;
static int g_session_dispatch_all_registered;

void zcs_rtmp_register(void);
void zcs_rtsp_register(void);
void zcs_http_register(void);
void zcs_rtp_ps_register(void);

void zcs_session_dispatch_register(const zcs_session_dispatch_ops *ops)
{
    int i;

    if (!ops || !ops->name || !ops->name[0]) {
        return;
    }
    for (i = 0; i < g_dispatch_count; ++i) {
        if (g_dispatch[i] && strcmp(g_dispatch[i]->name, ops->name) == 0) {
            g_dispatch[i] = ops;
            return;
        }
    }
    if (g_dispatch_count >= ZCS_SESSION_DISPATCH_MAX) {
        return;
    }
    g_dispatch[g_dispatch_count++] = ops;
}

void zcs_session_dispatch_register_all(void)
{
    if (g_session_dispatch_all_registered) {
        return;
    }
    g_session_dispatch_all_registered = 1;
    zcs_rtmp_register();
    zcs_rtsp_register();
    zcs_http_register();
    zcs_rtp_ps_register();
}

const zcs_session_dispatch_ops *zcs_session_dispatch_find(const char *name)
{
    int i;

    if (!name || !name[0]) {
        return NULL;
    }
    zcs_session_dispatch_register_all();
    for (i = 0; i < g_dispatch_count; ++i) {
        if (g_dispatch[i] && strcmp(g_dispatch[i]->name, name) == 0) {
            return g_dispatch[i];
        }
    }
    return NULL;
}

zn_err_t zcs_session_play_open_live(zcs_egress_source *play, zcs_media_source *src,
                                     zcs_session_live_mode mode)
{
    if (!play || !src) {
        return ZN_ERR_INVALID;
    }
    switch (mode) {
    case ZCS_SESSION_LIVE_GOP:
        return zcs_egress_source_open_live_gop(src, play);
    case ZCS_SESSION_LIVE_KEY:
        return zcs_egress_source_open_live_key(src, play);
    case ZCS_SESSION_LIVE_EDGE:
        return zcs_egress_source_open_live(src, 1, play);
    default:
        return ZN_ERR_INVALID;
    }
}

zn_err_t zcs_session_play_open_vod(zcs_egress_source *play, zcs_media_source *src,
                                    uint64_t seek_ms)
{
    zn_err_t err;

    if (!play || !src) {
        return ZN_ERR_INVALID;
    }
    err = zcs_egress_source_open_vod(src, play);
    if (err != ZN_OK) {
        return err;
    }
    if (seek_ms > 0) {
        (void)zcs_vod_source_seek_for_reader(src, play->readers.vod, seek_ms);
    }
    return ZN_OK;
}

void zcs_session_play_close(zcs_egress_source *play)
{
    if (play) {
        zcs_egress_source_close(play);
    }
}

zn_err_t zcs_session_attach_play(const char *protocol, void *session, zcs_media_source *src,
                                  const zcs_session_play_opts *opts)
{
    const zcs_session_dispatch_ops *ops;

    if (!protocol || !session || !src) {
        return ZN_ERR_INVALID;
    }
    ops = zcs_session_dispatch_find(protocol);
    if (!ops) {
        return ZN_ERR_INVALID;
    }
    if (zcs_media_source_is_vod(src)) {
        if (!ops->on_play_vod) {
            return ZN_ERR_INVALID;
        }
        return ops->on_play_vod(session, src, opts);
    }
    if (!ops->on_play_live) {
        return ZN_ERR_INVALID;
    }
    {
        zcs_session_cap_role role = zcs_session_capability_play_role(protocol);
        if (zcs_session_capability_check_source(role, src) != ZN_OK) {
            zcs_session_capability_log_reject(opts && opts->player ? opts->player : protocol, src,
                                              role);
            return ZN_ERR_NOT_IMPL;
        }
    }
    return ops->on_play_live(session, src, opts);
}

void zcs_session_detach_play(const char *protocol, void *session)
{
    const zcs_session_dispatch_ops *ops;

    if (!protocol || !session) {
        return;
    }
    ops = zcs_session_dispatch_find(protocol);
    if (ops && ops->on_teardown) {
        ops->on_teardown(session);
    }
}

zn_err_t zcs_session_attach_publish(const char *protocol, void *session, zcs_media_source *src,
                                     const zcs_session_publish_opts *opts)
{
    const zcs_session_dispatch_ops *ops;

    if (!protocol || !session || !src) {
        return ZN_ERR_INVALID;
    }
    ops = zcs_session_dispatch_find(protocol);
    if (!ops || !ops->on_publish) {
        return ZN_ERR_INVALID;
    }
    return ops->on_publish(session, src, opts);
}
