/**
 * @file parser.c
 * @brief HTTP 请求/响应解析。
 */
#include "zms/protocol/http/parser.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#endif

static void trim_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static const char *find_char(const char *s, size_t len, char c)
{
    for (size_t i = 0; i < len; ++i) {
        if (s[i] == c) {
            return s + i;
        }
    }
    return NULL;
}

static zn_err_t add_header(zms_http_parser *p, const char *key, size_t klen, const char *val,
                            size_t vlen)
{
    zms_http_header *h;
    char *value;

    if (!p || p->header_count >= ZMS_HTTP_HDR_MAX) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    if (klen >= ZMS_HTTP_KEY_MAX || vlen >= ZMS_HTTP_VAL_MAX) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    value = (char *)malloc(vlen + 1);
    if (!value) {
        return ZN_ERR_NOMEM;
    }
    memcpy(value, val, vlen);
    value[vlen] = '\0';
    trim_inplace(value);

    h = &p->headers[p->header_count];
    memcpy(h->key, key, klen);
    h->key[klen] = '\0';
    trim_inplace(h->key);
    h->value = value;
    p->header_count++;
    return ZN_OK;
}

void zms_http_parser_init(zms_http_parser *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

void zms_http_parser_clear(zms_http_parser *p)
{
    if (!p) {
        return;
    }
    for (size_t i = 0; i < p->header_count; ++i) {
        free(p->headers[i].value);
    }
    free(p->content);
    memset(p, 0, sizeof(*p));
}

static zn_err_t parse_lines(zms_http_parser *p, const char *buf, size_t size, size_t *body_off)
{
    const char *ptr = buf;
    const char *end = buf + size;

    while (ptr < end) {
        const char *line_end = find_char(ptr, (size_t)(end - ptr), '\n');
        if (!line_end) {
            return ZN_ERR_INVALID;
        }

        const char *line_stop = line_end;
        size_t line_len = (size_t)(line_stop - ptr);
        if (line_len > 0 && line_stop[-1] == '\r') {
            line_stop -= 1;
            line_len -= 1;
        }

        if (ptr == buf) {
            const char *sp1 = find_char(ptr, line_len, ' ');
            if (!sp1) {
                return ZN_ERR_INVALID;
            }
            size_t mlen = (size_t)(sp1 - ptr);
            if (mlen >= ZMS_HTTP_METHOD_MAX) {
                return ZN_ERR_INVALID;
            }
            memcpy(p->method, ptr, mlen);
            p->method[mlen] = '\0';

            const char *sp2 = find_char(sp1 + 1, (size_t)(line_stop - (sp1 + 1)), ' ');
            if (!sp2) {
                return ZN_ERR_INVALID;
            }

            size_t ulen = (size_t)(sp2 - (sp1 + 1));
            if (ulen >= ZMS_HTTP_URL_MAX) {
                return ZN_ERR_INVALID;
            }
            memcpy(p->url, sp1 + 1, ulen);
            p->url[ulen] = '\0';

            const char *q = strchr(p->url, '?');
            if (q) {
                size_t plen = (size_t)(q - p->url);
                strncpy(p->params, q + 1, sizeof(p->params) - 1);
                p->params[sizeof(p->params) - 1] = '\0';
                p->url[plen] = '\0';
            }

            size_t plen = (size_t)(line_stop - (sp2 + 1));
            if (plen >= ZMS_HTTP_PROTO_MAX) {
                return ZN_ERR_INVALID;
            }
            memcpy(p->protocol, sp2 + 1, plen);
            p->protocol[plen] = '\0';

            if (strncmp(p->method, "HTTP", 4) == 0) {
                p->is_response = 1;
            }
        } else if (line_len == 0) {
            ptr = line_end + 1;
            if (body_off) {
                *body_off = (size_t)(ptr - buf);
            }
            return ZN_OK;
        } else {
            const char *colon = find_char(ptr, line_len, ':');
            if (!colon) {
                return ZN_ERR_INVALID;
            }
            const char *vstart = colon + 1;
            while (vstart < line_stop && *vstart == ' ') {
                ++vstart;
            }
            zn_err_t err =
                add_header(p, ptr, (size_t)(colon - ptr), vstart, (size_t)(line_stop - vstart));
            if (err != ZN_OK) {
                return err;
            }
        }

        ptr = line_end + 1;
    }
    return ZN_ERR_INVALID;
}

zn_err_t zms_http_parser_parse_header(zms_http_parser *p, const char *buf, size_t size)
{
    if (!p || !buf || size < 4) {
        return ZN_ERR_INVALID;
    }
    zms_http_parser_clear(p);
    size_t body_off = 0;
    zn_err_t err = parse_lines(p, buf, size, &body_off);
    if (err != ZN_OK) {
        return err;
    }
    (void)body_off;
    return ZN_OK;
}

zn_err_t zms_http_parser_parse(zms_http_parser *p, const char *buf, size_t size)
{
    if (!p || !buf) {
        return ZN_ERR_INVALID;
    }
    zn_err_t err = zms_http_parser_parse_header(p, buf, size);
    if (err != ZN_OK) {
        return err;
    }

    size_t body_off = 0;
    const char *end_marker = NULL;
    for (size_t i = 0; i + 3 < size; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            body_off = i + 4;
            end_marker = buf + i;
            break;
        }
    }
    if (!end_marker) {
        return ZN_ERR_INVALID;
    }

    if (body_off < size) {
        return zms_http_parser_set_content(p, buf + body_off, size - body_off);
    }
    return ZN_OK;
}

zn_err_t zms_http_parser_set_content(zms_http_parser *p, const char *data, size_t len)
{
    if (!p) {
        return ZN_ERR_INVALID;
    }
    if (len == 0) {
        p->content_len = 0;
        return ZN_OK;
    }
    if (!data) {
        return ZN_ERR_INVALID;
    }
    size_t need = len + 1;
    if (p->content_cap < need) {
        char *nb = (char *)realloc(p->content, need);
        if (!nb) {
            return ZN_ERR_NOMEM;
        }
        p->content = nb;
        p->content_cap = need;
    }
    memcpy(p->content, data, len);
    p->content[len] = '\0';
    p->content_len = len;
    return ZN_OK;
}

const char *zms_http_parser_header(const zms_http_parser *p, const char *name)
{
    if (!p || !name) {
        return "";
    }
    for (size_t i = 0; i < p->header_count; ++i) {
        if (strcasecmp(p->headers[i].key, name) == 0) {
            return p->headers[i].value;
        }
    }
    return "";
}

const char *zms_http_parser_full_url(const zms_http_parser *p, char *out, size_t out_cap)
{
    if (!p || !out || out_cap == 0) {
        return "";
    }
    out[0] = '\0';
    if (p->params[0]) {
        snprintf(out, out_cap, "%s?%s", p->url, p->params);
    } else {
        strncpy(out, p->url, out_cap - 1);
        out[out_cap - 1] = '\0';
    }
    return out;
}
