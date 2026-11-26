/**
 * @file request_reader.c
 * @brief HTTP 请求体读取（按 Content-Length/chunked 收集）。
 */
#include "zms/protocol/http/request_reader.h"
#include "zms/protocol/http/parser.h"
#include "zms/protocol/http/message_framer.h"
#if !defined(_WIN32)
#include <strings.h>
#endif
#include <stdlib.h>
#include <string.h>

#define ZMS_HTTP_MAX_BODY (64U * 1024U)

struct zms_http_request_reader {
    zms_http_request_reader_opts opts;
    zms_http_message_framer *inner;
    zms_http_parser parser;
    char path_buf[ZMS_HTTP_URL_MAX + ZMS_HTTP_URL_MAX];
};

static void emit_request(zms_http_request_reader *s)
{
    if (!s || !s->opts.on_request) {
        return;
    }

    zms_http_request req;
    memset(&req, 0, sizeof(req));
    zms_msg_init(&req.msg);
    strncpy(req.method, s->parser.method, sizeof(req.method) - 1);

    zms_http_parser_full_url(&s->parser, s->path_buf, sizeof(s->path_buf));
    strncpy(req.path, s->path_buf, sizeof(req.path) - 1);

    req.body = s->parser.content;
    req.body_len = s->parser.content_len;

    /* 填充统一外壳：全部头字段 + body。视图指向本结构与解析器缓冲，仅回调期间有效。 */
    zms_msg_set_method(&req.msg, req.method, strlen(req.method));
    zms_msg_set_url(&req.msg, req.path, strlen(req.path));
    for (size_t i = 0; i < s->parser.header_count && i < ZMS_HTTP_HDR_MAX; ++i) {
        const zms_http_header *h = &s->parser.headers[i];

        (void)zms_msg_add_hdr(&req.msg, h->key, strlen(h->key), h->value,
                              h->value ? strlen(h->value) : 0);
    }
    zms_msg_set_body(&req.msg, req.body, req.body_len);

    s->opts.on_request(&req, s->opts.user);
}

int zms_http_req_is_ws_upgrade(const zms_http_request *req)
{
    const char *up = zms_http_req_header(req, "Upgrade");

    if (!up || !up[0]) {
        return 0;
    }
#if defined(_WIN32)
    return _stricmp(up, "websocket") == 0;
#else
    return strcasecmp(up, "websocket") == 0;
#endif
}

const char *zms_http_req_header(const zms_http_request *req, const char *name)
{
    const zms_kv *kv;

    if (!req || !name) {
        return "";
    }
    kv = zms_msg_find(&req->msg, name);
    return (kv && kv->value) ? kv->value : "";
}

static intptr_t on_packet_header(const char *header, size_t header_len, void *user)
{
    zms_http_request_reader *s = (zms_http_request_reader *)user;
    if (!s) {
        return 0;
    }

    zms_http_parser_clear(&s->parser);
    if (zms_http_parser_parse_header(&s->parser, header, header_len) != ZN_OK) {
        return 0;
    }

    const char *cl = zms_http_parser_header(&s->parser, "Content-Length");
    if (cl[0]) {
        unsigned long long n = 0;
        for (const char *p = cl; *p; ++p) {
            if (*p < '0' || *p > '9') {
                return 0;
            }
            n = n * 10ULL + (unsigned long long)(*p - '0');
            if (n > ZMS_HTTP_MAX_BODY) {
                return 0;
            }
        }
        if (n > 0) {
            return (intptr_t)n;
        }
    }

    emit_request(s);
    zms_http_parser_clear(&s->parser);
    return 0;
}

static void on_packet_content(const char *data, size_t len, void *user)
{
    zms_http_request_reader *s = (zms_http_request_reader *)user;
    if (!s) {
        return;
    }
    if (zms_http_parser_set_content(&s->parser, data, len) != ZN_OK) {
        return;
    }
    emit_request(s);
    zms_http_parser_clear(&s->parser);
}

zms_http_request_reader *zms_http_request_reader_create(const zms_http_request_reader_opts *opts)
{
    zms_http_request_reader *s = (zms_http_request_reader *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    if (opts) {
        s->opts = *opts;
    }

    zms_http_message_framer_opts iopts = {
        .on_header = on_packet_header,
        .on_content = on_packet_content,
        .user = s,
        .max_cache_size = 0,
    };
    s->inner = zms_http_message_framer_create(&iopts);
    if (!s->inner) {
        free(s);
        return NULL;
    }
    return s;
}

void zms_http_request_reader_destroy(zms_http_request_reader *s)
{
    if (!s) {
        return;
    }
    zms_http_message_framer_destroy(s->inner);
    zms_http_parser_clear(&s->parser);
    free(s);
}

void zms_http_request_reader_reset(zms_http_request_reader *s)
{
    if (!s) {
        return;
    }
    zms_http_message_framer_reset(s->inner);
    zms_http_parser_clear(&s->parser);
}

zn_err_t zms_http_request_reader_input(zms_http_request_reader *s, const void *data, size_t len)
{
    if (!s || !s->inner) {
        return ZN_ERR_INVALID;
    }
    return zms_http_message_framer_input(s->inner, data, len);
}
