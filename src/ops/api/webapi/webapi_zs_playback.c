/**
 * @file webapi_zs_playback.c
 * @brief 云存储槽位回放：start / stop。
 *
 * 槽位数据区是标准 MPEG-PS，file_reader 已按 .zslot 容器解析（含关键帧索引 seek）。
 * 这里只是把它注册成一个 VOD 媒体源，出流交给既有 egress（FLV/RTSP/WebRTC 等），
 * 平台/播放器按返回的 app/stream 拉流即可。
 *
 * 路径（A 族控制面，分组前缀 zstorage/）：
 *   /index/api/zstorage/playback/start  slot=<id> [channel=<key>] [start=<ms>] [app=] [stream=]
 *   /index/api/zstorage/playback/stop   app= stream=
 *
 * 拖动进度条由客户端 stop + start(start=<段内偏移>) 重建会话，不提供运行中 seek：
 * 正在出流的会话 reader 由各自 poller 驱动，跨线程改其 demux/fifo 不安全。
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zms/vod/io/source.h"
#include "zms/core/event.h"
#include "zms/core/stream/hub.h"
#include "zms/egress/recorder/zs_record_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 查找回放 VOD 源，兼容规范名（open 时流名会被规范化加后缀）。
static zms_media_source *zs_find_playback_src(const zms_webapi_query_map *q, const char *app)
{
    zms_media_source *src;

    src = zms_media_source_find_api_ex(ZMS_SCHEMA_RTMP, q->vhost, app, q->stream);
    if (src) {
        return src;
    }
    {
        char canon[ZMS_STREAM_MAX];

        canon[0] = '\0';
        zms_vod_canonical_stream(q->stream, canon, sizeof(canon));
        if (canon[0] && strcmp(canon, q->stream) != 0) {
            src = zms_media_source_find_api_ex(ZMS_SCHEMA_RTMP, q->vhost, app, canon);
        }
    }
    return src;
}

size_t webapi_handle_zs_playback_start(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                       int *http_status)
{
    zms_media_source *src;
    char path[512];
    char stream[ZMS_STREAM_MAX];
    const char *app;
    const char *slot_src;
    uint32_t slot;
    uint64_t start_ms = 0;
    uint64_t dur;

    *http_status = 200;
    slot_src = q->slot[0] ? q->slot : q->slots;
    if (!slot_src[0]) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing slot", NULL, body, body_cap);
    }
    slot = (uint32_t)strtoul(slot_src, NULL, 10);
    app = q->app[0] ? q->app : "playback";
    if (q->stream[0]) {
        snprintf(stream, sizeof(stream), "%s", q->stream);
    } else if (q->channel[0]) {
        snprintf(stream, sizeof(stream), "%s_%u", q->channel, slot);
    } else {
        snprintf(stream, sizeof(stream), "ps_%u", slot);
    }
    if (zms_zs_record_slot_path(slot, path, sizeof(path)) != 0) {
        return webapi_respond_json(ZMS_API_NOT_FOUND, "slot not found", NULL, body, body_cap);
    }
    src = zms_vod_source_open_vhost(q->vhost, app, stream, path, zms_media_events_poller());
    if (!src) {
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "open slot failed", NULL, body, body_cap);
    }
    if (q->start[0]) {
        start_ms = (uint64_t)strtoull(q->start, NULL, 10);
        if (start_ms > 0) {
            (void)zms_vod_source_seek_ms(src, start_ms);
        }
    }
    dur = zms_vod_source_duration_ms(src);
    return (size_t)snprintf(body, body_cap,
                            "{\"code\":0,\"msg\":\"success\",\"data\":{\"app\":\"%s\","
                            "\"stream\":\"%s\",\"slot\":%u,\"start\":%llu,\"duration\":%llu}}",
                            app, stream, slot, (unsigned long long)start_ms,
                            (unsigned long long)dur);
}

size_t webapi_handle_zs_playback_stop(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    zms_media_source *src;
    const char *app;

    *http_status = 200;
    app = q->app[0] ? q->app : "playback";
    if (!q->stream[0]) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing stream", NULL, body, body_cap);
    }
    src = zs_find_playback_src(q, app);
    if (src && zms_media_source_is_vod(src)) {
        zms_vod_source_stop(src);
    }
    return webapi_respond_json(ZMS_API_SUCCESS, "success", NULL, body, body_cap);
}
