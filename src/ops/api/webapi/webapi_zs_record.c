/**
 * @file webapi_zs_record.c
 * @brief 云存储录制任务与录像查询接口。
 *
 * 路径（A 族控制面，分组前缀 zstorage/）：
 *   /index/api/zstorage/record/start   channel=<key> [url=<rtsp>]
 *   /index/api/zstorage/record/stop    channel=<key>
 *   /index/api/zstorage/record/list
 *   /index/api/zstorage/record/query   [channel=<key>] start=<ms> end=<ms>
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zms/egress/recorder/zs_record_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t webapi_handle_zs_record_start(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status)
{
    zn_err_t rc;

    *http_status = 200;
    if (!q->channel[0]) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing channel", NULL, body, body_cap);
    }
    rc = zms_zs_record_start(q->channel, q->url[0] ? q->url : NULL);
    if (rc == ZN_ERR_INVALID) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "invalid channel/url", NULL, body, body_cap);
    }
    if (rc == ZN_ERR_STATE) {
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "storage disabled or pull not ready", NULL,
                                   body, body_cap);
    }
    if (rc != ZN_OK) {
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "start failed", NULL, body, body_cap);
    }
    return webapi_respond_json(ZMS_API_SUCCESS, "success", NULL, body, body_cap);
}

size_t webapi_handle_zs_record_stop(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                    int *http_status)
{
    *http_status = 200;
    if (!q->channel[0]) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing channel", NULL, body, body_cap);
    }
    zms_zs_record_stop(q->channel);
    return webapi_respond_json(ZMS_API_SUCCESS, "success", NULL, body, body_cap);
}

size_t webapi_handle_zs_record_list(zms_webapi_json_buf *jb)
{
    uint32_t n = zs_record_task_count();
    uint32_t i;
    int first = 1;

    if (webapi_jb_append(jb, "{\"code\":0,\"msg\":\"success\",\"data\":{\"total\":%u,\"items\":[",
                         n) != 0) {
        return 0;
    }
    for (i = 0; i < n; ++i) {
        zs_record_task_info info;

        if (!zs_record_task_at(i, &info)) {
            continue;
        }
        if (webapi_jb_append(jb,
                             "%s{\"channel\":\"%s\",\"url\":\"%s\",\"recording\":%d,\"online\":%d,"
                             "\"startedMs\":%llu}",
                             first ? "" : ",", info.channel_key, info.pull_url, info.recording,
                             info.online, (unsigned long long)info.started_utc_ms) != 0) {
            return 0;
        }
        first = 0;
    }
    if (webapi_jb_append(jb, "]}}") != 0) {
        return 0;
    }
    return jb->len;
}

size_t webapi_handle_zs_record_query(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status)
{
    zs_record_segment segs[256];
    uint64_t from_ms;
    uint64_t to_ms;
    uint32_t n;
    uint32_t i;
    size_t off;

    *http_status = 200;
    from_ms = q->start[0] ? (uint64_t)strtoull(q->start, NULL, 10) : 0;
    to_ms = q->end[0] ? (uint64_t)strtoull(q->end, NULL, 10) : 0;
    if (to_ms == 0) {
        return webapi_respond_json(ZMS_API_INVALID_ARGS, "missing end", NULL, body, body_cap);
    }
    n = zms_zs_record_query(q->channel[0] ? q->channel : NULL, from_ms, to_ms, segs,
                            (uint32_t)(sizeof(segs) / sizeof(segs[0])));

    off = (size_t)snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\",\"data\":{\"total\":%u,"
                                           "\"segments\":[",
                           n);
    for (i = 0; i < n && off < body_cap; ++i) {
        int w = snprintf(body + off, body_cap - off,
                         "%s{\"channel\":\"%s\",\"slot\":%u,\"start\":%llu,\"end\":%llu}",
                         i == 0 ? "" : ",", segs[i].channel_key, segs[i].slot_id,
                         (unsigned long long)segs[i].start_ms,
                         (unsigned long long)segs[i].end_ms);
        if (w < 0 || (size_t)w >= body_cap - off) {
            *http_status = 500;
            return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body,
                                       body_cap);
        }
        off += (size_t)w;
    }
    if (off < body_cap) {
        int w = snprintf(body + off, body_cap - off, "]}}");

        if (w > 0) {
            off += (size_t)w;
        }
    }
    return off;
}
