/**
 * @file url.c
 * @brief 媒体 URL 构造与播放后缀剥离。
 */
#include "zcs/core/stream/url.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/core/codec_filter.h"
#include "zcs/core/event.h"
#include "zcs/core/stream/vod_source_port.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define zcs_path_stricmp _stricmp
#else
#include <strings.h>
#define zcs_path_stricmp strcasecmp
#endif

void zcs_media_path_strip_play_suffix(char *path)
{
    if (!path || !path[0]) {
        return;
    }
    for (;;) {
        size_t len = strlen(path);
        if (zcs_vod_stream_is_vod_flv_wrap_suffix(path)) {
            path[len - 4] = '\0';
            continue;
        }
        if (len >= 5 && zcs_path_stricmp(path + len - 5, ".m3u8") == 0) {
            path[len - 5] = '\0';
            continue;
        }
        if (len >= 4 && zcs_path_stricmp(path + len - 4, ".mpd") == 0) {
            path[len - 4] = '\0';
            continue;
        }
        if (len >= 4 && zcs_path_stricmp(path + len - 4, ".flv") == 0) {
            if (zcs_vod_stream_is_native_flv_file(path)) {
                break;
            }
            path[len - 4] = '\0';
            continue;
        }
        if (len >= 4 && zcs_path_stricmp(path + len - 4, ".mp4") == 0) {
            path[len - 4] = '\0';
            continue;
        }
        if (len >= 3 && zcs_path_stricmp(path + len - 3, ".ts") == 0) {
            if (len >= 5 && path[len - 5] == '.' && path[len - 4] >= '0' && path[len - 4] <= '9') {
                break;
            }
            path[len - 3] = '\0';
            continue;
        }
        break;
    }
}

#define ZCS_URL_HOST "127.0.0.1"

static int fmt_srt_push(char *out, size_t cap, const zcs_media_server_ports *ports, const char *app,
                        const char *stream)
{
    if (!ports || !ports->srt || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "srt://%s:%u?streamid=#!::r=%s/%s,m=publish", ZCS_URL_HOST,
                    ports->srt, app, stream);
}

static int fmt_srt_play(char *out, size_t cap, const zcs_media_server_ports *ports, const char *app,
                        const char *stream)
{
    if (!ports || !ports->srt || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "srt://%s:%u?streamid=#!::r=%s/%s,m=request", ZCS_URL_HOST,
                    ports->srt, app, stream);
}

static int fmt_rtmp_push(char *out, size_t cap, const zcs_media_server_ports *ports,
                         const char *app, const char *stream)
{
    if (!ports || !ports->rtmp || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "rtmp://%s:%u/%s/%s", ZCS_URL_HOST, ports->rtmp, app, stream);
}

static int fmt_rtsp_record(char *out, size_t cap, const zcs_media_server_ports *ports,
                           const char *app, const char *stream)
{
    if (!ports || !ports->rtsp || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "rtsp://%s:%u/%s/%s", ZCS_URL_HOST, ports->rtsp, app, stream);
}

static int fmt_rtmp_play(char *out, size_t cap, const zcs_media_server_ports *ports,
                         const char *app, const char *stream)
{
    return fmt_rtmp_push(out, cap, ports, app, stream);
}

static int fmt_rtsp_play(char *out, size_t cap, const zcs_media_server_ports *ports,
                         const char *app, const char *stream)
{
    return fmt_rtsp_record(out, cap, ports, app, stream);
}

static int fmt_http_live_ts(char *out, size_t cap, const zcs_media_server_ports *ports,
                            const char *app, const char *stream)
{
    if (!ports || !ports->http || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "http://%s:%u/%s/%s.ts", ZCS_URL_HOST, ports->http, app, stream);
}

static int fmt_http_live_flv(char *out, size_t cap, const zcs_media_server_ports *ports,
                             const char *app, const char *stream)
{
    if (!ports || !ports->http || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "http://%s:%u/%s/%s.flv", ZCS_URL_HOST, ports->http, app, stream);
}

static int fmt_http_vod_flv(char *out, size_t cap, const zcs_media_server_ports *ports,
                            const char *app, const char *stream)
{
    if (!ports || !ports->http || !app || !stream) {
        return 0;
    }
    if (zcs_vod_stream_is_native_flv_file(stream)) {
        return snprintf(out, cap, "http://%s:%u/%s/%s", ZCS_URL_HOST, ports->http, app, stream);
    }
    if (zcs_vod_stream_is_vod_flv_wrap_suffix(stream)) {
        return snprintf(out, cap, "http://%s:%u/%s/%s", ZCS_URL_HOST, ports->http, app, stream);
    }
    if (zcs_vod_stream_has_disk_container_ext(stream)) {
        return snprintf(out, cap, "http://%s:%u/%s/%s.flv", ZCS_URL_HOST, ports->http, app, stream);
    }
    return snprintf(out, cap, "http://%s:%u/%s/%s.mp4.flv", ZCS_URL_HOST, ports->http, app, stream);
}

static int fmt_http_mp4(char *out, size_t cap, const zcs_media_server_ports *ports, const char *app,
                        const char *stream)
{
    if (!ports || !ports->http || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "http://%s:%u/%s/%s", ZCS_URL_HOST, ports->http, app, stream);
}

static int fmt_live_hls(char *out, size_t cap, const zcs_media_server_ports *ports, const char *app,
                        const char *stream)
{
    if (!ports || !ports->http || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "http://%s:%u/%s/%s.m3u8", ZCS_URL_HOST, ports->http, app, stream);
}

static int fmt_live_dash(char *out, size_t cap, const zcs_media_server_ports *ports,
                         const char *app, const char *stream)
{
    if (!ports || !ports->http || !app || !stream) {
        return 0;
    }
    return snprintf(out, cap, "http://%s:%u/%s/%s.mpd", ZCS_URL_HOST, ports->http, app, stream);
}

static int fmt_vod_hls(char *out, size_t cap, const zcs_media_server_ports *ports, const char *app,
                       const char *stream)
{
    char m3u8_rel[ZCS_STREAM_MAX];

    if (!ports || !ports->http || !app || !stream) {
        return 0;
    }
    if (!zcs_vod_mp4_stream_to_m3u8_rel(stream, m3u8_rel, sizeof(m3u8_rel))) {
        return 0;
    }
    return snprintf(out, cap, "http://%s:%u/%s/%s", ZCS_URL_HOST, ports->http, app, m3u8_rel);
}

static void url_rewrite_host(char *url, size_t cap, const char *vhost)
{
    const char *scheme;
    const char *host;
    const char *end;
    char tmp[ZCS_MEDIA_URL_MAX];

    if (!url || !url[0] || !vhost || !vhost[0] || strncmp(url, "srt://", 6) == 0) {
        return;
    }
    scheme = strstr(url, "://");
    if (!scheme) {
        return;
    }
    host = scheme + 3;
    end = host;
    while (*end && *end != ':' && *end != '/') {
        ++end;
    }
    if ((size_t)(scheme - url) + 3 + strlen(vhost) + strlen(end) + 1 > cap) {
        return;
    }
    snprintf(tmp, sizeof(tmp), "%.*s://%s%s", (int)(scheme - url), url, vhost, end);
    strncpy(url, tmp, cap - 1);
    url[cap - 1] = 0;
}

void zcs_media_urls_build(zcs_media_urls *out, const zcs_media_source *src,
                          const zcs_media_server_ports *ports)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!src || !ports) {
        return;
    }

    if (zcs_media_source_is_vod(src)) {
        char canon[ZCS_STREAM_MAX];
        zcs_vod_canonical_stream(src->stream, canon, sizeof(canon));
        if (!canon[0]) {
            strncpy(canon, src->stream, sizeof(canon) - 1);
        }
        canon[sizeof(canon) - 1] = '\0';

        fmt_rtmp_play(out->rtmp_play, sizeof(out->rtmp_play), ports, src->app, canon);
        fmt_rtsp_play(out->rtsp_play, sizeof(out->rtsp_play), ports, src->app, canon);
        fmt_http_vod_flv(out->http_flv, sizeof(out->http_flv), ports, src->app, canon);
        fmt_http_mp4(out->http_mp4, sizeof(out->http_mp4), ports, src->app, canon);
        fmt_vod_hls(out->hls, sizeof(out->hls), ports, src->app, canon);
    } else {
        char play_stream[ZCS_STREAM_MAX];
        const char *play = src->stream_requested[0] ? src->stream_requested : src->stream;

        strncpy(play_stream, play, sizeof(play_stream) - 1);
        play_stream[sizeof(play_stream) - 1] = '\0';
        zcs_media_path_strip_play_suffix(play_stream);

        if (zcs_session_capability_check_source(ZCS_PROTO_CAP_RTMP_PLAY, src) == ZN_OK) {
            fmt_rtmp_play(out->rtmp_play, sizeof(out->rtmp_play), ports, src->app, play_stream);
        }
        fmt_rtsp_play(out->rtsp_play, sizeof(out->rtsp_play), ports, src->app, play_stream);
        if (zcs_session_capability_check_source(ZCS_PROTO_CAP_HTTP_FLV_PLAY, src) == ZN_OK) {
            fmt_http_live_flv(out->http_flv, sizeof(out->http_flv), ports, src->app, play_stream);
        }
        if (zcs_session_capability_check_source(ZCS_PROTO_CAP_HTTP_TS_PLAY, src) == ZN_OK) {
            fmt_http_live_ts(out->http_ts, sizeof(out->http_ts), ports, src->app, play_stream);
        }
        if (zcs_session_capability_check_source(ZCS_PROTO_CAP_HLS_PLAY, src) == ZN_OK) {
            fmt_live_hls(out->hls, sizeof(out->hls), ports, src->app, play_stream);
        }
        if (zcs_session_capability_check_source(ZCS_PROTO_CAP_DASH_PLAY, src) == ZN_OK) {
            fmt_live_dash(out->dash, sizeof(out->dash), ports, src->app, play_stream);
        }
    }

    {
        const char *origin_stream = src->stream_requested[0] ? src->stream_requested : src->stream;

        switch (src->publish_origin) {
        case ZCS_ORIGIN_RTSP_PUSH:
            fmt_rtsp_record(out->origin, sizeof(out->origin), ports, src->app, origin_stream);
            break;
        case ZCS_ORIGIN_SRT_PUSH:
            fmt_srt_push(out->origin, sizeof(out->origin), ports, src->app, origin_stream);
            break;
        case ZCS_ORIGIN_RTMP_PUSH:
        default:
            fmt_rtmp_push(out->origin, sizeof(out->origin), ports, src->app, origin_stream);
            break;
        }
        if (!out->origin[0]) {
            fmt_rtmp_push(out->origin, sizeof(out->origin), ports, src->app, origin_stream);
        }
    }
    if (zcs_vhost_enabled() && strcmp(src->vhost, ZCS_DEFAULT_VHOST) != 0) {
        url_rewrite_host(out->origin, sizeof(out->origin), src->vhost);
        url_rewrite_host(out->rtmp_play, sizeof(out->rtmp_play), src->vhost);
        url_rewrite_host(out->rtsp_play, sizeof(out->rtsp_play), src->vhost);
        url_rewrite_host(out->http_flv, sizeof(out->http_flv), src->vhost);
        url_rewrite_host(out->http_ts, sizeof(out->http_ts), src->vhost);
        url_rewrite_host(out->http_mp4, sizeof(out->http_mp4), src->vhost);
        url_rewrite_host(out->hls, sizeof(out->hls), src->vhost);
        url_rewrite_host(out->dash, sizeof(out->dash), src->vhost);
    }
}

/* 运行期调试用：所有 URL debug 级别打印，不影响生产日志 */
void zcs_media_urls_log_stream(const zcs_media_source *src, const zcs_media_server_ports *ports)
{
    if (!src || !ports) {
        return;
    }
    zcs_media_urls u;
    zcs_media_urls_build(&u, src, ports);
    zn_debug("[source] stream_urls app=%s stream=%s video=%d audio=%d origin=%s", src->app,
              src->stream, src->has_video, src->has_audio, u.origin[0] ? u.origin : "?");
    if (u.origin[0]) {
        zn_debug("[source] url_push %s", u.origin);
    }
    if (u.rtmp_play[0]) {
        zn_debug("[source] url_rtmp_play %s", u.rtmp_play);
    }
    if (u.rtsp_play[0]) {
        zn_debug("[source] url_rtsp_play %s", u.rtsp_play);
    }
    if (u.http_flv[0]) {
        zn_debug("[source] url_http_flv %s", u.http_flv);
    }
    if (zcs_media_source_is_vod(src) && u.http_mp4[0]) {
        zn_debug("[source] url_http_mp4 %s", u.http_mp4);
    }
    if (u.hls[0]) {
        zn_debug("[source] url_hls %s", u.hls);
    }
    if (u.dash[0]) {
        zn_debug("[source] url_dash %s", u.dash);
    }
    if (src->publish_origin == ZCS_ORIGIN_RTSP_PUSH && u.rtsp_play[0]) {
        zn_debug("[source] url_rtsp_record %s", u.rtsp_play);
    }
    if (src->publish_origin == ZCS_ORIGIN_SRT_PUSH && u.origin[0]) {
        zn_debug("[source] url_srt_push %s", u.origin);
    }
    if (!zcs_media_source_is_vod(src) && ports->srt) {
        char play_stream[ZCS_STREAM_MAX];
        char srt_play[ZCS_MEDIA_URL_MAX];

        strncpy(play_stream, src->stream_requested[0] ? src->stream_requested : src->stream,
                sizeof(play_stream) - 1);
        play_stream[sizeof(play_stream) - 1] = '\0';
        zcs_media_path_strip_play_suffix(play_stream);
        if (zcs_session_capability_check_source(ZCS_PROTO_CAP_SRT_PLAY, src) == ZN_OK &&
            fmt_srt_play(srt_play, sizeof(srt_play), ports, src->app, play_stream)) {
            zn_debug("[source] url_srt_play %s", srt_play);
        }
    }
}

/* 推流建立时调用一次：debug 级别打印全部拉流地址（info 摘要见 media_event.c） */
void zcs_media_urls_log_publish(const zcs_media_source *src, const zcs_media_server_ports *ports)
{
    if (!src || !ports) {
        return;
    }
    zcs_media_urls u;
    zcs_media_urls_build(&u, src, ports);

    if (src->stream_requested[0] && strcmp(src->stream_requested, src->stream) != 0) {
        zn_debug("[source] publish_urls app=%s client_stream=%s stream_id=%s push=%s", src->app,
                  src->stream_requested, src->stream, u.origin[0] ? u.origin : "-");
    } else {
        zn_debug("[source] publish_urls app=%s stream=%s push=%s", src->app, src->stream,
                  u.origin[0] ? u.origin : "-");
    }

    if (src->has_video) {
        zn_debug("[source] video codec=%s %dx%d", zcs_codec_name(src->video.codec),
                  (int)src->video.width, (int)src->video.height);
    } else {
        zn_debug("[source] video -");
    }

    if (src->has_audio) {
        zn_debug("[source] audio codec=%s %dHz ch=%d", zcs_codec_name(src->audio.codec),
                  (int)src->audio.sample_rate, (int)src->audio.channels);
    } else {
        zn_debug("[source] audio -");
    }

    if (u.origin[0]) {
        zn_debug("[source] url_push %s", u.origin);
    }
    if (u.rtmp_play[0]) {
        zn_debug("[source] url_rtmp_play %s", u.rtmp_play);
    }
    if (u.rtsp_play[0]) {
        zn_debug("[source] url_rtsp_play %s", u.rtsp_play);
    }
    if (u.http_flv[0]) {
        zn_debug("[source] url_http_flv %s", u.http_flv);
    }
    if (zcs_media_source_is_vod(src) && u.http_mp4[0]) {
        zn_debug("[source] url_http_mp4 %s", u.http_mp4);
    }
    if (u.hls[0]) {
        zn_debug("[source] url_hls %s", u.hls);
    }
    if (u.dash[0]) {
        zn_debug("[source] url_dash %s", u.dash);
    }
    if (!zcs_media_source_is_vod(src) && ports->srt) {
        char play_stream[ZCS_STREAM_MAX];
        char srt_play[ZCS_MEDIA_URL_MAX];

        strncpy(play_stream, src->stream_requested[0] ? src->stream_requested : src->stream,
                sizeof(play_stream) - 1);
        play_stream[sizeof(play_stream) - 1] = '\0';
        zcs_media_path_strip_play_suffix(play_stream);
        if (zcs_session_capability_check_source(ZCS_PROTO_CAP_SRT_PLAY, src) == ZN_OK &&
            fmt_srt_play(srt_play, sizeof(srt_play), ports, src->app, play_stream)) {
            zn_debug("[source] url_srt_play %s", srt_play);
        }
    }
}
