/**
 * @file gb28181_api.c
 * @brief /index/api/gb28181/{status,channels,alarm}。
 */
#include "ops/api/webapi/gb28181_api.h"

#include "zms/device/gb28181/gb28181.h"
#include "zms/device/gb28181/gb28181_platform.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
} jb_t;

static void jb_putc(jb_t *j, char c)
{
    if (j->len + 1 < j->cap) {
        j->buf[j->len++] = c;
        j->buf[j->len] = '\0';
    }
}

static void jb_puts(jb_t *j, const char *s)
{
    while (s && *s) {
        jb_putc(j, *s++);
    }
}

static void jb_fmt(jb_t *j, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (j->len + 1 >= j->cap) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        j->len += (size_t)n < j->cap - j->len ? (size_t)n : j->cap - j->len - 1;
    }
}

static void jb_esc(jb_t *j, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");

    while (*p) {
        switch (*p) {
        case '"':
            jb_puts(j, "\\\"");
            break;
        case '\\':
            jb_puts(j, "\\\\");
            break;
        case '\n':
            jb_puts(j, "\\n");
            break;
        case '\r':
            jb_puts(j, "\\r");
            break;
        case '\t':
            jb_puts(j, "\\t");
            break;
        default:
            if (*p < 0x20) {
                jb_fmt(j, "\\u%04x", *p);
            } else {
                jb_putc(j, (char)*p);
            }
            break;
        }
        ++p;
    }
}

static int handle_status(jb_t *j)
{
    jb_fmt(j,
           "{\"code\":0,\"msg\":\"success\",\"data\":{\"running\":%d,\"registered\":%d,"
           "\"sessions\":%d}}",
           zms_gb28181_running(), zms_gb28181_registered(), zms_gb28181_session_count());
    return (int)j->len;
}

static int handle_channels(jb_t *j)
{
    /* 通道快照几十 KB：堆上取，别放 poller 栈。 */
    zms_gb28181_channel_info *ch =
        (zms_gb28181_channel_info *)calloc(ZMS_GB28181_CHANNEL_MAX, sizeof(*ch));
    size_t n;
    size_t i;
    int rc = 0;

    if (!ch) {
        return -1;
    }
    n = zms_gb28181_channels(ch, ZMS_GB28181_CHANNEL_MAX);

    jb_puts(j, "{\"code\":0,\"msg\":\"success\",\"data\":[");
    for (i = 0; i < n; ++i) {
        jb_puts(j, i ? ",{\"id\":\"" : "{\"id\":\"");
        jb_esc(j, ch[i].id);
        jb_puts(j, "\",\"name\":\"");
        jb_esc(j, ch[i].name);
        jb_puts(j, "\",\"vhost\":\"");
        jb_esc(j, ch[i].vhost);
        jb_puts(j, "\",\"app\":\"");
        jb_esc(j, ch[i].app);
        jb_puts(j, "\",\"stream\":\"");
        jb_esc(j, ch[i].stream);
        jb_fmt(j, "\",\"online\":%d}", ch[i].online);
    }
    jb_puts(j, "]}");
    rc = (int)j->len;
    free(ch);
    return rc;
}

static int handle_platform_devices(jb_t *j)
{
    zms_gb28181_platform_device *devs =
        (zms_gb28181_platform_device *)calloc(ZMS_GB_PLATFORM_CHANNEL_MAX, sizeof(*devs));
    size_t n;
    size_t i;
    int rc;

    if (!devs) {
        return -1;
    }
    n = zms_gb28181_platform_devices(devs, ZMS_GB_PLATFORM_CHANNEL_MAX);

    jb_puts(j, "{\"code\":0,\"msg\":\"success\",\"data\":[");
    for (i = 0; i < n; ++i) {
        jb_puts(j, i ? ",{\"id\":\"" : "{\"id\":\"");
        jb_esc(j, devs[i].id);
        jb_puts(j, "\",\"ip\":\"");
        jb_esc(j, devs[i].ip);
        jb_fmt(j, "\",\"port\":%u,\"online\":%d,\"channels\":%d,\"name\":\"",
               devs[i].port, devs[i].online, devs[i].channel_count);
        jb_esc(j, devs[i].name);
        jb_puts(j, "\",\"manufacturer\":\"");
        jb_esc(j, devs[i].manufacturer);
        jb_puts(j, "\",\"model\":\"");
        jb_esc(j, devs[i].model);
        jb_puts(j, "\",\"firmware\":\"");
        jb_esc(j, devs[i].firmware);
        jb_puts(j, "\"}");
    }
    jb_puts(j, "]}");
    rc = (int)j->len;
    free(devs);
    return rc;
}

static int handle_platform_channels(const zms_gb28181_api_req *req, jb_t *j)
{
    zms_gb28181_platform_channel *chs =
        (zms_gb28181_platform_channel *)calloc(ZMS_GB_PLATFORM_CHANNEL_MAX, sizeof(*chs));
    const char *device = req->device && req->device[0] ? req->device : req->channel;
    size_t n;
    size_t i;
    int rc;

    if (!chs) {
        return -1;
    }
    if (!device || !device[0]) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"missing device\"}");
        free(chs);
        return (int)j->len;
    }
    n = zms_gb28181_platform_channels(device, chs, ZMS_GB_PLATFORM_CHANNEL_MAX);
    jb_puts(j, "{\"code\":0,\"msg\":\"success\",\"data\":[");
    for (i = 0; i < n; ++i) {
        jb_puts(j, i ? ",{\"id\":\"" : "{\"id\":\"");
        jb_esc(j, chs[i].id);
        jb_puts(j, "\",\"device_id\":\"");
        jb_esc(j, chs[i].device_id);
        jb_puts(j, "\",\"name\":\"");
        jb_esc(j, chs[i].name);
        jb_fmt(j, "\",\"online\":%d}", chs[i].online);
    }
    jb_puts(j, "]}");
    rc = (int)j->len;
    free(chs);
    return rc;
}

static int handle_platform_query(const zms_gb28181_api_req *req, jb_t *j)
{
    int rc;

    if (!req->device || !req->device[0] || !req->cmd || !req->cmd[0]) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"missing device/cmd\"}");
        return (int)j->len;
    }
    rc = zms_gb28181_platform_query(req->device, req->cmd);
    if (rc != 0) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"query not sent\"}");
        return (int)j->len;
    }
    jb_puts(j, "{\"code\":0,\"msg\":\"success\"}");
    return (int)j->len;
}

static int handle_platform_sessions(jb_t *j)
{
    zms_gb28181_platform_session *s =
        (zms_gb28181_platform_session *)calloc(ZMS_GB_PLATFORM_CHANNEL_MAX, sizeof(*s));
    size_t n;
    size_t i;
    int rc;

    if (!s) {
        return -1;
    }
    n = zms_gb28181_platform_sessions(s, ZMS_GB_PLATFORM_CHANNEL_MAX);

    jb_puts(j, "{\"code\":0,\"msg\":\"success\",\"data\":[");
    for (i = 0; i < n; ++i) {
        jb_puts(j, i ? ",{\"call_id\":\"" : "{\"call_id\":\"");
        jb_esc(j, s[i].call_id);
        jb_puts(j, "\",\"device_id\":\"");
        jb_esc(j, s[i].device_id);
        jb_puts(j, "\",\"channel_id\":\"");
        jb_esc(j, s[i].channel_id);
        jb_puts(j, "\",\"vhost\":\"");
        jb_esc(j, s[i].vhost);
        jb_puts(j, "\",\"app\":\"");
        jb_esc(j, s[i].app);
        jb_puts(j, "\",\"stream\":\"");
        jb_esc(j, s[i].stream);
        jb_fmt(j, "\",\"ssrc\":%u,\"port\":%u,\"active\":%d}", s[i].ssrc,
               s[i].local_port, s[i].active);
    }
    jb_puts(j, "]}");
    rc = (int)j->len;
    free(s);
    return rc;
}

static int handle_platform_invite(const zms_gb28181_api_req *req, jb_t *j)
{
    const char *device = req->device;
    const char *channel = req->channel;
    int rc;

    if (!device || !device[0] || !channel || !channel[0]) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"missing device/channel\"}");
        return (int)j->len;
    }
    rc = zms_gb28181_platform_invite(device, channel, req->app, req->stream, req->tcp);
    if (rc != 0) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"invite failed\"}");
        return (int)j->len;
    }
    jb_puts(j, "{\"code\":0,\"msg\":\"success\"}");
    return (int)j->len;
}

static int handle_platform_bye(const zms_gb28181_api_req *req, jb_t *j)
{
    if (!req->call_id || !req->call_id[0]) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"missing call_id\"}");
        return (int)j->len;
    }
    if (zms_gb28181_platform_bye(req->call_id) != 0) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"bye failed\"}");
        return (int)j->len;
    }
    jb_puts(j, "{\"code\":0,\"msg\":\"success\"}");
    return (int)j->len;
}

static int handle_platform_ptz(const zms_gb28181_api_req *req, jb_t *j)
{
    if (!req->device || !req->device[0] || !req->channel || !req->channel[0]) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"missing device/channel\"}");
        return (int)j->len;
    }
    if (zms_gb28181_platform_ptz(req->device, req->channel, req->pan, req->tilt, req->zoom,
                                 req->stop) != 0) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"ptz not sent\"}");
        return (int)j->len;
    }
    jb_puts(j, "{\"code\":0,\"msg\":\"success\"}");
    return (int)j->len;
}

static int handle_platform_record_query(const zms_gb28181_api_req *req, jb_t *j)
{
    if (!req->device || !req->device[0] || !req->channel || !req->channel[0] || !req->start ||
        !req->end) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"missing device/channel/start/end\"}");
        return (int)j->len;
    }
    if (zms_gb28181_platform_record_query(req->device, req->channel, req->start, req->end) != 0) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"query not sent\"}");
        return (int)j->len;
    }
    jb_puts(j, "{\"code\":0,\"msg\":\"success\"}");
    return (int)j->len;
}

static int handle_platform_records(const zms_gb28181_api_req *req, jb_t *j)
{
    zms_gb28181_platform_record *rec =
        (zms_gb28181_platform_record *)calloc(32, sizeof(*rec));
    size_t n;
    size_t i;
    int rc;

    if (!rec) {
        return -1;
    }
    if (!req->device || !req->device[0]) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"missing device\"}");
        free(rec);
        return (int)j->len;
    }
    n = zms_gb28181_platform_records(req->device, req->channel, rec, 32);
    jb_puts(j, "{\"code\":0,\"msg\":\"success\",\"data\":[");
    for (i = 0; i < n; ++i) {
        jb_puts(j, i ? ",{\"channel_id\":\"" : "{\"channel_id\":\"");
        jb_esc(j, rec[i].channel_id);
        jb_puts(j, "\",\"name\":\"");
        jb_esc(j, rec[i].name);
        jb_puts(j, "\",\"file_path\":\"");
        jb_esc(j, rec[i].file_path);
        jb_puts(j, "\",\"start_time\":\"");
        jb_esc(j, rec[i].start_time);
        jb_puts(j, "\",\"end_time\":\"");
        jb_esc(j, rec[i].end_time);
        jb_puts(j, "\",\"type\":\"");
        jb_esc(j, rec[i].type);
        jb_puts(j, "\"}");
    }
    jb_puts(j, "]}");
    rc = (int)j->len;
    free(rec);
    return rc;
}

static int handle_platform_alarms(jb_t *j)
{
    zms_gb28181_platform_alarm *a =
        (zms_gb28181_platform_alarm *)calloc(32, sizeof(*a));
    size_t n;
    size_t i;
    int rc;

    if (!a) {
        return -1;
    }
    n = zms_gb28181_platform_alarms(a, 32);

    jb_puts(j, "{\"code\":0,\"msg\":\"success\",\"data\":[");
    for (i = 0; i < n; ++i) {
        jb_puts(j, i ? ",{\"device_id\":\"" : "{\"device_id\":\"");
        jb_esc(j, a[i].device_id);
        jb_puts(j, "\",\"channel_id\":\"");
        jb_esc(j, a[i].channel_id);
        jb_fmt(j, "\",\"priority\":%d,\"method\":%d,\"time_ms\":%llu,\"description\":\"",
               a[i].priority, a[i].method, (unsigned long long)a[i].time_ms);
        jb_esc(j, a[i].description);
        jb_puts(j, "\"}");
    }
    jb_puts(j, "]}");
    rc = (int)j->len;
    free(a);
    return rc;
}

static int handle_alarm(const zms_gb28181_api_req *req, jb_t *j)
{
    int rc = zms_gb28181_alarm(req->channel, req->priority > 0 ? req->priority : 1,
                               req->method > 0 ? req->method : 5,
                               req->description ? req->description : "alarm");
    if (rc != 0) {
        jb_puts(j, "{\"code\":-1,\"msg\":\"alarm not sent (not registered?)\"}");
        return (int)j->len;
    }
    jb_puts(j, "{\"code\":0,\"msg\":\"success\"}");
    return (int)j->len;
}

int zms_webapi_gb28181_handle(const zms_gb28181_api_req *req, int *http_status, char *body,
                              size_t body_cap)
{
    jb_t j;

    if (!req || !req->endpoint || !body || body_cap == 0) {
        return -1;
    }
    if (http_status) {
        *http_status = 200;
    }
    j.buf = body;
    j.cap = body_cap;
    j.len = 0;
    body[0] = '\0';

    if (strcmp(req->endpoint, "status") == 0) {
        return handle_status(&j);
    }
    if (strcmp(req->endpoint, "channels") == 0) {
        return handle_channels(&j);
    }
    if (strcmp(req->endpoint, "alarm") == 0) {
        return handle_alarm(req, &j);
    }
    if (strcmp(req->endpoint, "platform/devices") == 0) {
        return handle_platform_devices(&j);
    }
    if (strcmp(req->endpoint, "platform/channels") == 0) {
        return handle_platform_channels(req, &j);
    }
    if (strcmp(req->endpoint, "platform/query") == 0) {
        return handle_platform_query(req, &j);
    }
    if (strcmp(req->endpoint, "platform/invite") == 0) {
        return handle_platform_invite(req, &j);
    }
    if (strcmp(req->endpoint, "platform/bye") == 0) {
        return handle_platform_bye(req, &j);
    }
    if (strcmp(req->endpoint, "platform/sessions") == 0) {
        return handle_platform_sessions(&j);
    }
    if (strcmp(req->endpoint, "platform/ptz") == 0) {
        return handle_platform_ptz(req, &j);
    }
    if (strcmp(req->endpoint, "platform/record_query") == 0) {
        return handle_platform_record_query(req, &j);
    }
    if (strcmp(req->endpoint, "platform/records") == 0) {
        return handle_platform_records(req, &j);
    }
    if (strcmp(req->endpoint, "platform/alarms") == 0) {
        return handle_platform_alarms(&j);
    }
    return -1;
}
