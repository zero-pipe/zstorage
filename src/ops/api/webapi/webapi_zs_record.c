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
#include "zcs/egress/recorder/zs_record_task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t webapi_handle_zs_record_start(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status)
{
    zn_err_t rc;

    *http_status = 200;
    if (!q->channel[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing channel", NULL, body, body_cap);
    }
    rc = zcs_zs_record_start(q->channel, q->url[0] ? q->url : NULL);
    if (rc == ZN_ERR_INVALID) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "invalid channel/url", NULL, body, body_cap);
    }
    if (rc == ZN_ERR_STATE) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "storage disabled or pull not ready", NULL,
                                   body, body_cap);
    }
    if (rc != ZN_OK) {
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "start failed", NULL, body, body_cap);
    }
    return webapi_respond_json(ZCS_API_SUCCESS, "success", NULL, body, body_cap);
}

size_t webapi_handle_zs_record_stop(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                    int *http_status)
{
    *http_status = 200;
    if (!q->channel[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing channel", NULL, body, body_cap);
    }
    zcs_zs_record_stop(q->channel);
    return webapi_respond_json(ZCS_API_SUCCESS, "success", NULL, body, body_cap);
}

size_t webapi_handle_zs_record_list(zcs_webapi_json_buf *jb)
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

size_t webapi_handle_zs_record_query(const zcs_webapi_query_map *q, char *body, size_t body_cap,
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
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing end", NULL, body, body_cap);
    }
    n = zcs_zs_record_query(q->channel[0] ? q->channel : NULL, from_ms, to_ms, segs,
                            (uint32_t)(sizeof(segs) / sizeof(segs[0])));

    off = (size_t)snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\",\"data\":{\"total\":%u,"
                                           "\"segments\":[",
                           n);
    for (i = 0; i < n && off < body_cap; ++i) {
        int w = snprintf(body + off, body_cap - off,
                         "%s{\"channel\":\"%s\",\"slot\":%u,\"start\":%llu,\"end\":%llu,\"size\":%llu}",
                         i == 0 ? "" : ",", segs[i].channel_key, segs[i].slot_id,
                         (unsigned long long)segs[i].start_ms,
                         (unsigned long long)segs[i].end_ms,
                         (unsigned long long)segs[i].bytes);
        if (w < 0 || (size_t)w >= body_cap - off) {
            *http_status = 500;
            return webapi_respond_json(ZCS_API_OTHER_FAILED, "response too large", NULL, body,
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

size_t webapi_handle_zs_record_delete(const zcs_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status)
{
    uint32_t slots[256];
    uint32_t n = 0;
    uint32_t deleted;
    const char *p;

    *http_status = 200;
    if (!q->slots[0]) {
        return webapi_respond_json(ZCS_API_INVALID_ARGS, "missing slots", NULL, body, body_cap);
    }
    /* 解析 CSV 槽位号。 */
    p = q->slots;
    while (*p && n < (uint32_t)(sizeof(slots) / sizeof(slots[0]))) {
        char *end = NULL;
        unsigned long v = strtoul(p, &end, 10);

        if (end == p) {
            break;
        }
        slots[n++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
        if (*end == '\0') {
            break;
        }
    }
    deleted = zcs_zs_record_delete(slots, n);
    {
        char data[64];

        snprintf(data, sizeof(data), "{\"deleted\":%u,\"requested\":%u}", deleted, n);
        return webapi_respond_json(ZCS_API_SUCCESS, "success", data, body, body_cap);
    }
}

size_t webapi_handle_zs_node_status(char *body, size_t body_cap, int *http_status)
{
    uint32_t slots = 0;
    uint32_t free_slots = 0;
    uint64_t used_bytes = 0;
    uint64_t cap_bytes = 0;
    int w;

    *http_status = 200;
    zcs_zs_record_node_status(&slots, &free_slots, &used_bytes, &cap_bytes);
    w = snprintf(body, body_cap,
                 "{\"code\":0,\"msg\":\"success\",\"data\":{\"slots\":%u,\"freeSlots\":%u,"
                 "\"usedGb\":%llu,\"capacityGb\":%llu,\"recordingTasks\":%u}}",
                 slots, free_slots,
                 (unsigned long long)(used_bytes / (1024ull * 1024ull * 1024ull)),
                 (unsigned long long)(cap_bytes / (1024ull * 1024ull * 1024ull)),
                 zs_record_task_count());
    if (w < 0 || (size_t)w >= body_cap) {
        *http_status = 500;
        return webapi_respond_json(ZCS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
    }
    return (size_t)w;
}
