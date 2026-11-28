/**
 * @file parser.c
 * @brief RTSP 消息解析与构造（请求行/状态行/头/CSeq）。
 */
#include "zcs/protocol/rtsp/parser.h"
#include "util/strtok_r.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

static const char *method_names[] = {
    "OPTIONS",  "DESCRIBE",      "SETUP",         "PLAY",     "PAUSE",
    "TEARDOWN", "GET_PARAMETER", "SET_PARAMETER", "ANNOUNCE", "RECORD",
};

static zcs_rtsp_method parse_method(const char *s)
{
    for (size_t i = 0; i < sizeof(method_names) / sizeof(method_names[0]); ++i) {
        if (strcasecmp(s, method_names[i]) == 0) {
            return (zcs_rtsp_method)i;
        }
    }
    return ZCS_RTSP_UNKNOWN;
}

void zcs_rtsp_message_clear(zcs_rtsp_message *msg)
{
    if (!msg) {
        return;
    }
    zcs_msg_fini(&msg->msg);
    memset(msg, 0, sizeof(*msg));
}

static zn_err_t add_header(zcs_rtsp_message *msg, const char *line)
{
    const char *p = strchr(line, ':');
    const char *vstart;
    size_t klen;
    size_t vlen;
    const char *key;
    const char *val;

    if (!p) {
        return ZN_ERR_INVALID;
    }
    klen = (size_t)(p - line);
    while (klen > 0 && isspace((unsigned char)line[klen - 1])) {
        --klen;
    }
    if (klen == 0 || klen >= ZCS_RTSP_KEY_MAX) {
        return ZN_ERR_INVALID;
    }

    vstart = p + 1;
    while (*vstart == ' ' || *vstart == '\t') {
        ++vstart;
    }
    vlen = strlen(vstart);
    while (vlen > 0 && (vstart[vlen - 1] == '\r' || vstart[vlen - 1] == '\n' ||
                        vstart[vlen - 1] == ' ' || vstart[vlen - 1] == '\t')) {
        --vlen;
    }
    if (vlen >= ZCS_RTSP_VAL_MAX) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }

    /* 报文缓冲是临时的：键与值都登记为消息自有副本。 */
    key = zcs_msg_own_copy(&msg->msg, line, klen);
    val = zcs_msg_own_copy(&msg->msg, vstart, vlen);
    if (!key || !val) {
        return ZN_ERR_NOMEM;
    }
    if (zcs_msg_add_hdr(&msg->msg, key, klen, val, vlen) != 0) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    return ZN_OK;
}

static const char *find_header_end(const char *data, size_t len, size_t *header_len)
{
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            if (header_len) {
                *header_len = i + 4;
            }
            return data + i;
        }
    }
    return NULL;
}

static zn_err_t parse_header_block(char *block, zcs_rtsp_message *msg)
{
    char *save = NULL;
    char *line = zcs_strtok_r(block, "\r\n", &save);
    if (!line) {
        return ZN_ERR_INVALID;
    }

    if (strncmp(line, "RTSP/", 5) == 0) {
        char ver[16];
        char reason[128];
        const char *owned;

        reason[0] = '\0';
        if (sscanf(line, "RTSP/%15s %d %127[^\r\n]", ver, &msg->status_code, reason) < 2) {
            return ZN_ERR_INVALID;
        }
        strncpy(msg->version, ver, sizeof(msg->version) - 1);
        owned = zcs_msg_own_copy(&msg->msg, reason, strlen(reason));
        if (!owned) {
            return ZN_ERR_NOMEM;
        }
        msg->msg.kind = ZCS_MSG_KIND_RESPONSE;
        msg->msg.reason = owned;
        msg->msg.reason_len = strlen(reason);
    } else {
        char method[64];
        char url[512];
        char ver[16];
        const char *owned;

        if (sscanf(line, "%63s %511s RTSP/%15s", method, url, ver) < 3) {
            return ZN_ERR_INVALID;
        }
        msg->method = parse_method(method);
        strncpy(msg->version, ver, sizeof(msg->version) - 1);
        owned = zcs_msg_own_copy(&msg->msg, url, strlen(url));
        if (!owned) {
            return ZN_ERR_NOMEM;
        }
        msg->msg.kind = ZCS_MSG_KIND_REQUEST;
        (void)zcs_msg_set_method(&msg->msg, owned, strlen(url));
        (void)zcs_msg_set_url(&msg->msg, owned, strlen(url));
    }

    while ((line = zcs_strtok_r(NULL, "\r\n", &save)) != NULL) {
        if (line[0] == '\0') {
            break;
        }
        zn_err_t err = add_header(msg, line);
        if (err != ZN_OK) {
            return err;
        }
    }
    return ZN_OK;
}

zn_err_t zcs_rtsp_message_parse_header(const char *data, size_t len, zcs_rtsp_message *msg)
{
    if (!data || !msg || len == 0) {
        return ZN_ERR_INVALID;
    }

    zcs_rtsp_message_clear(msg);
    size_t hdr_len = 0;
    if (!find_header_end(data, len, &hdr_len)) {
        return ZN_ERR_AGAIN;
    }

    char block[ZCS_RTSP_LINE_MAX];
    if (hdr_len >= sizeof(block)) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(block, data, hdr_len);
    block[hdr_len] = '\0';
    return parse_header_block(block, msg);
}

zn_err_t zcs_rtsp_message_parse(const char *data, size_t len, zcs_rtsp_message *msg,
                                 size_t *consumed)
{
    if (!data || !msg || len == 0) {
        return ZN_ERR_INVALID;
    }

    zn_err_t err = zcs_rtsp_message_parse_header(data, len, msg);
    if (err != ZN_OK) {
        return err;
    }

    size_t hdr_len = 0;
    find_header_end(data, len, &hdr_len);

    int content_len = zcs_rtsp_message_content_length(msg);
    size_t total = hdr_len + (content_len > 0 ? (size_t)content_len : 0);
    if (len < total) {
        return ZN_ERR_AGAIN;
    }

    if (content_len > 0) {
        (void)zcs_msg_set_body(&msg->msg, data + hdr_len, (size_t)content_len);
    }

    if (consumed) {
        *consumed = total;
    }
    return ZN_OK;
}

const char *zcs_rtsp_message_get(const zcs_rtsp_message *msg, const char *key)
{
    const zcs_kv *kv;

    if (!msg || !key) {
        return NULL;
    }
    kv = zcs_msg_find(&msg->msg, key);
    return kv ? kv->value : NULL;
}

int zcs_rtsp_message_content_length(const zcs_rtsp_message *msg)
{
    const char *v = zcs_rtsp_message_get(msg, "Content-Length");
    if (!v || !v[0]) {
        return 0;
    }
    return atoi(v);
}

zn_err_t zcs_rtsp_message_build_request(char *out, size_t cap, zcs_rtsp_method method,
                                         const char *url, unsigned cseq, const char *session,
                                         const char *extra_headers, const char *body,
                                         size_t body_len, size_t *out_len)
{
    if (!out || !url || method < 0 || method >= ZCS_RTSP_UNKNOWN) {
        return ZN_ERR_INVALID;
    }

    const char *m = method_names[method];
    int n = snprintf(out, cap,
                     "%s %s RTSP/1.0\r\n"
                     "CSeq: %u\r\n"
                     "User-Agent: ZN-RTSP/1.0\r\n",
                     m, url, cseq);
    if (n < 0 || (size_t)n >= cap) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }

    if (session && session[0]) {
        n += snprintf(out + n, cap - (size_t)n, "Session: %s\r\n", session);
        if (n < 0 || (size_t)n >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (extra_headers && extra_headers[0]) {
        n += snprintf(out + n, cap - (size_t)n, "%s", extra_headers);
        if (n < 0 || (size_t)n >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (body && body_len > 0) {
        n += snprintf(out + n, cap - (size_t)n, "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)n >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
    }
    n += snprintf(out + n, cap - (size_t)n, "\r\n");
    if (n < 0 || (size_t)n >= cap) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }

    if (body && body_len > 0) {
        if ((size_t)n + body_len >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(out + n, body, body_len);
        n += (int)body_len;
    }

    if (out_len) {
        *out_len = (size_t)n;
    }
    return ZN_OK;
}

zn_err_t zcs_rtsp_message_build_response(char *out, size_t cap, int status, const char *reason,
                                          unsigned cseq, const char *session,
                                          const char *extra_headers, const char *body,
                                          size_t body_len, size_t *out_len)
{
    if (!out || !reason) {
        return ZN_ERR_INVALID;
    }

    int n = snprintf(out, cap,
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %u\r\n"
                     "Server: ZCS-RTSP/1.0\r\n",
                     status, reason, cseq);
    if (n < 0 || (size_t)n >= cap) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }

    if (session && session[0]) {
        n += snprintf(out + n, cap - (size_t)n, "Session: %s\r\n", session);
        if (n < 0 || (size_t)n >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (extra_headers && extra_headers[0]) {
        n += snprintf(out + n, cap - (size_t)n, "%s", extra_headers);
        if (n < 0 || (size_t)n >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
    }
    if (body && body_len > 0) {
        n += snprintf(out + n, cap - (size_t)n, "Content-Length: %zu\r\n", body_len);
        if (n < 0 || (size_t)n >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
    }
    n += snprintf(out + n, cap - (size_t)n, "\r\n");
    if (n < 0 || (size_t)n >= cap) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }

    if (body && body_len > 0) {
        if ((size_t)n + body_len >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        memcpy(out + n, body, body_len);
        n += (int)body_len;
    }

    if (out_len) {
        *out_len = (size_t)n;
    }
    return ZN_OK;
}
