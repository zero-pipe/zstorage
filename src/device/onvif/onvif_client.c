/**
 * @file onvif_client.c
 * @brief ONVIF 客户端：设备信息、Media1 profiles/GetStreamUri、PTZ。
 */
#include "device/onvif/onvif_internal.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int xml_safe(const char *s)
{
    return s && s[0] && !strpbrk(s, "<>&=\"'");
}

static void derive_media_xaddr(const char *device_xaddr, char *out, size_t cap)
{
    const char *p = strstr(device_xaddr, "/onvif/device_service");
    if (p) {
        size_t prefix = (size_t)(p - device_xaddr);
        const char *suffix = "/onvif/media_service";
        if (prefix + strlen(suffix) + 1 <= cap) {
            memcpy(out, device_xaddr, prefix);
            memcpy(out + prefix, suffix, strlen(suffix) + 1);
            return;
        }
    }
    snprintf(out, cap, "%s", device_xaddr);
}

/** 调用一次 SOAP，返回 malloc 的响应体（失败 NULL） */
static char *onvif_call_body(zms_onvif_client *c, const char *xaddr, const char *body)
{
    char *resp = NULL;
    if (zms_onvif_call(c, xaddr, body, &resp) != 0) {
        return NULL;
    }
    return resp;
}

static void read_device_information(zms_onvif_client *c)
{
    char *body = onvif_call_body(c, c->dev.xaddr, "<tds:GetDeviceInformation/>");
    if (!body) {
        return;
    }
    (void)zms_xml_text(body, "Manufacturer", c->dev.manufacturer, sizeof(c->dev.manufacturer));
    (void)zms_xml_text(body, "Model", c->dev.model, sizeof(c->dev.model));
    (void)zms_xml_text(body, "FirmwareVersion", c->dev.firmware, sizeof(c->dev.firmware));
    (void)zms_xml_text(body, "SerialNumber", c->dev.serial, sizeof(c->dev.serial));
    (void)zms_xml_text(body, "HardwareId", c->dev.hardware, sizeof(c->dev.hardware));
    free(body);
}

static void read_capabilities(zms_onvif_client *c)
{
    char *body = onvif_call_body(
        c, c->dev.xaddr,
        "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>");
    if (!body) {
        derive_media_xaddr(c->dev.xaddr, c->dev.media_xaddr, sizeof(c->dev.media_xaddr));
        return;
    }
    {
        const char *media = zms_xml_find(body, "Media");
        if (media) {
            (void)zms_xml_text(media, "XAddr", c->dev.media_xaddr, sizeof(c->dev.media_xaddr));
        }
        if (c->dev.media_xaddr[0] == '\0') {
            derive_media_xaddr(c->dev.xaddr, c->dev.media_xaddr, sizeof(c->dev.media_xaddr));
        }
        {
            const char *ptz = zms_xml_find(body, "PTZ");
            if (ptz) {
                (void)zms_xml_text(ptz, "XAddr", c->dev.ptz_xaddr, sizeof(c->dev.ptz_xaddr));
            }
        }
    }
    free(body);
}

zms_onvif_client *zms_onvif_connect(const char *device_xaddr, const char *user, const char *pass,
                                    int timeout_ms)
{
    zms_onvif_client *c;

    if (!device_xaddr || !device_xaddr[0]) {
        return NULL;
    }
    c = (zms_onvif_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    snprintf(c->dev.xaddr, sizeof(c->dev.xaddr), "%s", device_xaddr);
    if (user) {
        snprintf(c->user, sizeof(c->user), "%s", user);
    }
    if (pass) {
        snprintf(c->pass, sizeof(c->pass), "%s", pass);
    }
    c->timeout_ms = timeout_ms > 0 ? timeout_ms : 10000;

    read_device_information(c);
    read_capabilities(c);
    if (c->dev.media_xaddr[0] == '\0') {
        zn_warn("onvif: no media xaddr for %s", device_xaddr);
    }
    return c;
}

int zms_onvif_refresh(zms_onvif_client *c, zms_onvif_device *out)
{
    if (!c) {
        return -1;
    }
    read_device_information(c);
    read_capabilities(c);
    if (out) {
        *out = c->dev;
    }
    return c->dev.media_xaddr[0] ? 0 : -1;
}

static int get_stream_uri(zms_onvif_client *c, const char *token, char *uri, size_t cap)
{
    char body[512];
    char *resp;
    int rc;

    if (!xml_safe(token) || !c->dev.media_xaddr[0]) {
        return -1;
    }
    rc = snprintf(body, sizeof(body),
                  "<trt:GetStreamUri>"
                  "<trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream>"
                  "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
                  "</trt:StreamSetup>"
                  "<trt:ProfileToken>%s</trt:ProfileToken>"
                  "</trt:GetStreamUri>",
                  token);
    if (rc <= 0 || (size_t)rc >= sizeof(body)) {
        return -1;
    }
    resp = onvif_call_body(c, c->dev.media_xaddr, body);
    if (!resp) {
        return -1;
    }
    rc = zms_xml_text(resp, "Uri", uri, cap);
    free(resp);
    return rc;
}

int zms_onvif_get_profiles(zms_onvif_client *c, zms_onvif_profile *out, size_t cap, size_t *count)
{
    char *body;
    const char *cur;
    size_t n = 0;

    if (count) {
        *count = 0;
    }
    if (!c || !out || cap == 0 || !c->dev.media_xaddr[0]) {
        return -1;
    }
    body = onvif_call_body(c, c->dev.media_xaddr, "<trt:GetProfiles/>");
    if (!body) {
        return -1;
    }

    cur = body;
    while (n < cap) {
        const char *tag = zms_xml_find_tag_start(cur, NULL, "Profiles");
        const char *block;
        const char *next;
        const char *block_end;
        zms_onvif_profile *p;

        if (!tag) {
            break;
        }
        block = strchr(tag, '>');
        if (!block) {
            break;
        }
        ++block;
        next = zms_xml_find_tag_start(block, NULL, "Profiles");
        block_end = next;

        p = &out[n];
        memset(p, 0, sizeof(*p));
        (void)zms_xml_attr(tag, "Profiles", "token", p->token, sizeof(p->token));
        (void)zms_xml_text_between(block, block_end, "Name", p->name, sizeof(p->name));
        (void)zms_xml_text_between(block, block_end, "Encoding", p->encoding, sizeof(p->encoding));
        {
            const char *res = zms_xml_find_between(block, block_end, "Resolution");
            if (res) {
                char tmp[32];
                if (zms_xml_text_between(res, block_end, "Width", tmp, sizeof(tmp)) == 0) {
                    p->width = atoi(tmp);
                }
                if (zms_xml_text_between(res, block_end, "Height", tmp, sizeof(tmp)) == 0) {
                    p->height = atoi(tmp);
                }
            }
        }
        {
            char tmp[32];
            if (zms_xml_text_between(block, block_end, "FrameRateLimit", tmp, sizeof(tmp)) == 0) {
                p->fps = (int)(atof(tmp) + 0.5);
            }
        }
        if (p->token[0]) {
            (void)get_stream_uri(c, p->token, p->uri, sizeof(p->uri));
        }
        ++n;
        if (!next) {
            break;
        }
        cur = next;
    }
    free(body);
    if (count) {
        *count = n;
    }
    return n > 0 ? 0 : -1;
}

const zms_onvif_device *zms_onvif_device_info(const zms_onvif_client *c)
{
    return c ? &c->dev : NULL;
}

int zms_onvif_ptz_move(zms_onvif_client *c, const char *profile_token, float pan, float tilt,
                       float zoom)
{
    char body[512];
    char *resp;
    int rc;

    if (!c || !xml_safe(profile_token) || !c->dev.ptz_xaddr[0]) {
        return -1;
    }
    rc = snprintf(body, sizeof(body),
                  "<tptz:ContinuousMove>"
                  "<tptz:ProfileToken>%s</tptz:ProfileToken>"
                  "<tptz:Velocity><tt:PanTilt x=\"%.3f\" y=\"%.3f\"/><tt:Zoom x=\"%.3f\"/>"
                  "</tptz:Velocity>"
                  "</tptz:ContinuousMove>",
                  profile_token, pan, tilt, zoom);
    if (rc <= 0 || (size_t)rc >= sizeof(body)) {
        return -1;
    }
    resp = onvif_call_body(c, c->dev.ptz_xaddr, body);
    if (!resp) {
        return -1;
    }
    free(resp);
    return 0;
}

int zms_onvif_ptz_stop(zms_onvif_client *c, const char *profile_token)
{
    char body[256];
    char *resp;
    int rc;

    if (!c || !xml_safe(profile_token) || !c->dev.ptz_xaddr[0]) {
        return -1;
    }
    rc = snprintf(body, sizeof(body),
                  "<tptz:Stop><tptz:ProfileToken>%s</tptz:ProfileToken>"
                  "<tptz:PanTilt>true</tptz:PanTilt><tptz:Zoom>true</tptz:Zoom></tptz:Stop>",
                  profile_token);
    if (rc <= 0 || (size_t)rc >= sizeof(body)) {
        return -1;
    }
    resp = onvif_call_body(c, c->dev.ptz_xaddr, body);
    if (!resp) {
        return -1;
    }
    free(resp);
    return 0;
}

void zms_onvif_close(zms_onvif_client *c)
{
    free(c);
}

static int uri_put_encoded(char **w, const char *end, const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            if (*w + 1 >= end) {
                return -1;
            }
            *(*w)++ = (char)ch;
        } else {
            if (*w + 3 >= end) {
                return -1;
            }
            *(*w)++ = '%';
            *(*w)++ = hex[ch >> 4];
            *(*w)++ = hex[ch & 0x0f];
        }
    }
    return 0;
}

int zms_onvif_uri_with_auth(const char *uri, const char *user, const char *pass, char *out,
                            size_t cap)
{
    const char *scheme_end;
    const char *host_start;
    const char *slash;
    char *w;
    char *end;

    if (!uri || !out || cap == 0) {
        return -1;
    }
    if (!user || !user[0] || !pass) {
        snprintf(out, cap, "%s", uri);
        return 0;
    }
    scheme_end = strstr(uri, "://");
    if (!scheme_end) {
        snprintf(out, cap, "%s", uri);
        return 0;
    }
    host_start = scheme_end + 3;
    slash = strchr(host_start, '/');
    if (!slash) {
        slash = host_start + strlen(host_start);
    }
    if (memchr(host_start, '@', (size_t)(slash - host_start)) != NULL) {
        snprintf(out, cap, "%s", uri);
        return 0;
    }
    if ((size_t)(host_start - uri) >= cap) {
        return -1;
    }
    memcpy(out, uri, (size_t)(host_start - uri));
    w = out + (host_start - uri);
    end = out + cap - 1;
    if (uri_put_encoded(&w, end, user) != 0 || w >= end) {
        goto fail;
    }
    *w++ = ':';
    if (uri_put_encoded(&w, end, pass) != 0 || w >= end) {
        goto fail;
    }
    *w++ = '@';
    {
        /* 必须保留 host:port 部分（从 host_start 复制，而不是从 path） */
        size_t rest = strlen(host_start);
        if ((size_t)(end - w) < rest) {
            goto fail;
        }
        memcpy(w, host_start, rest + 1);
    }
    return 0;

fail:
    out[0] = '\0';
    return -1;
}
