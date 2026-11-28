/**
 * @file device_api.c
 * @brief /index/api/device/*：能力、事件与 PTZ 的统一设备视图。
 */
#include "ops/api/webapi/device_api.h"

#include "zcs/device/core/device_core.h"
#include "zcs/device/core/device_event.h"
#include "zcs/device/core/device_ptz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t json_esc(char *out, size_t cap, size_t len, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");

    while (*p) {
        if (len + 2 >= cap) {
            break;
        }
        if (*p == '"' || *p == '\\') {
            out[len++] = '\\';
            out[len++] = (char)*p;
        } else if (*p == '\n') {
            out[len++] = '\\';
            out[len++] = 'n';
        } else if (*p >= 0x20) {
            out[len++] = (char)*p;
        }
        ++p;
    }
    out[len] = '\0';
    return len;
}

int zcs_webapi_device_events(char *body, size_t body_cap)
{
    zcs_device_event *evs = (zcs_device_event *)calloc(32, sizeof(*evs));
    size_t n;
    size_t i;
    size_t len;

    if (!body || body_cap == 0) {
        return 0;
    }
    if (!evs) {
        return 0;
    }
    n = zcs_device_event_list(evs, 32);
    len = (size_t)snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\",\"data\":[");
    for (i = 0; i < n && len + 64 < body_cap; ++i) {
        len += (size_t)snprintf(body + len, body_cap - len,
                                "%s{\"type\":\"%s\",\"device_id\":\"", i ? "," : "",
                                zcs_device_event_type_name(evs[i].type));
        len = json_esc(body, body_cap, len, evs[i].device_id);
        len += (size_t)snprintf(body + len, body_cap - len, "\",\"channel_id\":\"");
        len = json_esc(body, body_cap, len, evs[i].channel_id);
        len += (size_t)snprintf(body + len, body_cap - len,
                                "\",\"priority\":%d,\"time_ms\":%llu,\"message\":\"",
                                evs[i].priority, (unsigned long long)evs[i].time_ms);
        len = json_esc(body, body_cap, len, evs[i].message);
        len += (size_t)snprintf(body + len, body_cap - len, "\"}");
    }
    if (len + 3 < body_cap) {
        len += (size_t)snprintf(body + len, body_cap - len, "]}");
    }
    free(evs);
    return (int)len;
}

int zcs_webapi_device_ptz(const char *channel, float pan, float tilt, float zoom, int stop,
                          char *body, size_t body_cap)
{
    zcs_device_ptz_cmd cmd;

    if (!channel || !channel[0]) {
        return snprintf(body, body_cap, "{\"code\":-1,\"msg\":\"missing channel\"}");
    }
    memset(&cmd, 0, sizeof(cmd));
    snprintf(cmd.channel_id, sizeof(cmd.channel_id), "%s", channel);
    cmd.pan = pan;
    cmd.tilt = tilt;
    cmd.zoom = zoom;
    cmd.stop = stop;
    if (zcs_device_ptz_dispatch(&cmd) != 0) {
        return snprintf(body, body_cap, "{\"code\":-1,\"msg\":\"ptz backend failed\"}");
    }
    return snprintf(body, body_cap, "{\"code\":0,\"msg\":\"success\"}");
}

int zcs_webapi_device_capabilities(char *body, size_t body_cap)
{
    unsigned caps;
    int n;

    if (!body || body_cap == 0) {
        return 0;
    }
    caps = zcs_device_core_caps();
    n = snprintf(body, body_cap,
                 "{\"code\":0,\"msg\":\"success\",\"data\":{"
                 "\"live\":%d,\"playback\":%d,\"ptz\":%d,\"snapshot\":%d,"
                 "\"events\":%d,\"record_query\":%d,\"channels\":%u}}",
                 (caps & ZCS_DEV_CAP_LIVE) ? 1 : 0,
                 (caps & ZCS_DEV_CAP_PLAYBACK) ? 1 : 0,
                 (caps & ZCS_DEV_CAP_PTZ) ? 1 : 0,
                 (caps & ZCS_DEV_CAP_SNAPSHOT) ? 1 : 0,
                 (caps & ZCS_DEV_CAP_EVENTS) ? 1 : 0,
                 (caps & ZCS_DEV_CAP_RECORD_QUERY) ? 1 : 0,
                 (unsigned)zcs_device_core_count(0, 0));
    return n > 0 ? n : 0;
}
