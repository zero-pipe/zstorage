/**
 * @file webapi_record.c
 * @brief Web API 录制文件操作：loadMP4 / deleteRecord / seek / speed。
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zcs/vod/io/source.h"
#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stddef.h>
#include <string.h>

size_t webapi_handle_load_mp4(const zcs_webapi_query_map *q, const zcs_web_api_opts *opts,
                                   char *body, size_t body_cap, int *http_status)
{
    zcs_media_source *src;
    zn_poller *pol;
    const char *app;
    const char *stream;

    *http_status = 200;
    app = q->app[0] ? q->app : "mp4_record";
    stream = q->stream[0] ? q->stream : NULL;
    if (!stream || !q->file_path[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing stream/file_path", NULL, body, body_cap);
    }
    if (!zcs_mp4_recorder_path_under_root(q->file_path)) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "file_path not under www/record or www", NULL,
                            body, body_cap);
    }
    {
        struct stat st;
        if (stat(q->file_path, &st) != 0 || st.st_size <= 0) {
            return webapi_respond_json(ZCS_API_NOT_FOUND, "file not found", NULL, body, body_cap);
        }
    }
    pol = opts && opts->poller ? opts->poller : zcs_media_events_poller();
    if (!pol) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "no poller", NULL, body, body_cap);
    }
    src = zcs_media_source_find_api_ex(ZCS_SCHEMA_RTMP, q->vhost, app, stream);
    if (!src) {
        /* registered VOD streams carry the canonical media suffix (.mp4/.flv/...) */
        char canon[ZCS_STREAM_MAX];
        zcs_vod_canonical_stream(stream, canon, sizeof(canon));
        if (canon[0] && strcmp(canon, stream) != 0) {
            src = zcs_media_source_find_api_ex(ZCS_SCHEMA_RTMP, q->vhost, app, canon);
        }
    }
    if (src && zcs_media_source_is_vod(src)) {
        /* idempotent: reopen would replace the reader and drop its FLV index */
        return webapi_respond_json(ZCS_API_SUCCESS, "success", NULL, body, body_cap);
    }
    src = zcs_vod_source_open_vhost(q->vhost, app, stream, q->file_path, pol);
    if (!src) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "loadMP4File failed", NULL, body, body_cap);
    }
    return webapi_respond_json(ZCS_API_SUCCESS, "success", NULL, body, body_cap);
}

size_t webapi_handle_delete_record(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                        int *http_status)
{
    *http_status = 200;
    if (!q->file_path[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing file_path", NULL, body, body_cap);
    }
    if (!zcs_mp4_recorder_path_under_root(q->file_path)) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "file_path not under www/record or www", NULL,
                            body, body_cap);
    }
    if (remove(q->file_path) != 0) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "delete failed", NULL, body, body_cap);
    }
    return webapi_respond_json(ZCS_API_SUCCESS, "success", NULL, body, body_cap);
}

size_t webapi_handle_seek_record(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                       int *http_status)
{
    zcs_media_source *src;
    const char *app;
    const char *stream;
    uint64_t stamp_ms;
    uint64_t actual;

    *http_status = 200;
    app = q->app[0] ? q->app : "mp4_record";
    stream = q->stream[0] ? q->stream : NULL;
    if (!stream || !q->stamp[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing stream/stamp", NULL, body, body_cap);
    }
    stamp_ms = (uint64_t)strtod(q->stamp, NULL);
    src = zcs_media_source_find_api_ex(q->schema[0] ? q->schema : ZCS_SCHEMA_RTMP, q->vhost, app,
                                       stream);
    if (!src) {
        src = zcs_media_source_find_api_ex(ZCS_SCHEMA_RTMP, q->vhost, app, stream);
    }
    if (!src || !zcs_media_source_is_vod(src)) {
        return webapi_respond_json(ZCS_API_NOT_FOUND, "can not find the stream", NULL, body, body_cap);
    }
    actual = zcs_vod_source_seek_ms(src, stamp_ms);
    return (size_t)snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\",\"data\":%llu}",
                            (unsigned long long)actual);
}

size_t webapi_handle_record_speed(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    (void)q;
    *http_status = 200;
    return webapi_respond_json(ZCS_API_SUCCESS, "success", NULL, body, body_cap);
}