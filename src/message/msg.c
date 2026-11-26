/**
 * @file msg.c
 * @brief 协议消息外壳实现（零拷贝视图 + 键值查找）。
 */
#include "zms/message/msg.h"

#include <stdlib.h>
#include <string.h>

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int str_ieq(const char *a, const char *b, size_t blen)
{
    size_t i = 0;

    for (; i < blen; ++i) {
        int ca = (unsigned char)a[i];

        if (ca == '\0') {
            return 0;
        }
        if (ascii_lower(ca) != ascii_lower((unsigned char)b[i])) {
            return 0;
        }
    }
    return a[i] == '\0';
}

static int is_space(int c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void trim_view(const char **p, size_t *len)
{
    const char *s = *p;
    size_t n = *len;

    while (n > 0 && is_space((unsigned char)s[0])) {
        ++s;
        --n;
    }
    while (n > 0 && is_space((unsigned char)s[n - 1])) {
        --n;
    }
    *p = s;
    *len = n;
}

void zms_msg_init(zms_msg *m)
{
    if (m) {
        memset(m, 0, sizeof(*m));
    }
}

const char *zms_msg_own_copy(zms_msg *m, const char *s, size_t len)
{
    char *copy;

    if (!m || (!s && len > 0)) {
        return NULL;
    }
    if (m->owned_n == m->owned_cap) {
        int cap = m->owned_cap ? m->owned_cap * 2 : 8;
        char **nb = (char **)realloc(m->owned, (size_t)cap * sizeof(*nb));

        if (!nb) {
            return NULL;
        }
        m->owned = nb;
        m->owned_cap = cap;
    }
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    if (len > 0) {
        memcpy(copy, s, len);
    }
    copy[len] = '\0';
    m->owned[m->owned_n++] = copy;
    return copy;
}

void zms_msg_fini(zms_msg *m)
{
    int i;

    if (!m) {
        return;
    }
    for (i = 0; i < m->owned_n; ++i) {
        free(m->owned[i]);
    }
    free(m->owned);
    memset(m, 0, sizeof(*m));
}

int zms_msg_set_method(zms_msg *m, const char *method, size_t len)
{
    if (!m || !method) {
        return -1;
    }
    trim_view(&method, &len);
    m->method = method;
    m->method_len = len;
    return 0;
}

int zms_msg_set_url(zms_msg *m, const char *url, size_t len)
{
    if (!m || !url) {
        return -1;
    }
    m->url = url;
    m->url_len = len;
    return 0;
}

int zms_msg_set_status(zms_msg *m, int status, const char *reason, size_t len)
{
    if (!m) {
        return -1;
    }
    m->kind = ZMS_MSG_KIND_RESPONSE;
    m->status = status;
    m->reason = reason;
    m->reason_len = len;
    return 0;
}

int zms_msg_set_body(zms_msg *m, const char *body, size_t len)
{
    if (!m) {
        return -1;
    }
    m->body = body;
    m->body_len = body ? len : 0;
    return 0;
}

int zms_msg_add_hdr(zms_msg *m, const char *name, size_t name_len, const char *value,
                    size_t value_len)
{
    zms_kv *kv;

    if (!m || !name || m->hdr_n >= ZMS_MSG_HDR_MAX) {
        return -1;
    }
    trim_view(&name, &name_len);
    trim_view(&value, &value_len);
    if (name_len == 0) {
        return -1;
    }
    kv = &m->hdr[m->hdr_n++];
    kv->name = name;
    kv->name_len = name_len;
    kv->value = value ? value : "";
    kv->value_len = value ? value_len : 0;
    return 0;
}

const zms_kv *zms_msg_find(const zms_msg *m, const char *name)
{
    if (!m || !name) {
        return NULL;
    }
    for (int i = 0; i < m->hdr_n; ++i) {
        if (str_ieq(name, m->hdr[i].name, m->hdr[i].name_len)) {
            return &m->hdr[i];
        }
    }
    return NULL;
}

int zms_msg_get_copy(const zms_msg *m, const char *name, char *out, size_t cap)
{
    const zms_kv *kv;

    if (!out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    kv = zms_msg_find(m, name);
    if (!kv) {
        return -1;
    }
    return zms_kv_copy_value(kv, out, cap);
}

int zms_kv_copy_name(const zms_kv *kv, char *out, size_t cap)
{
    if (!kv || !out || cap == 0) {
        return -1;
    }
    if (kv->name_len >= cap) {
        out[0] = '\0';
        return -1;
    }
    memcpy(out, kv->name, kv->name_len);
    out[kv->name_len] = '\0';
    return 0;
}

int zms_kv_copy_value(const zms_kv *kv, char *out, size_t cap)
{
    if (!kv || !out || cap == 0) {
        return -1;
    }
    if (kv->value_len >= cap) {
        out[0] = '\0';
        return -1;
    }
    memcpy(out, kv->value, kv->value_len);
    out[kv->value_len] = '\0';
    return 0;
}

int zms_msg_get_int(const zms_msg *m, const char *name, int64_t def, int64_t *out)
{
    const zms_kv *kv = zms_msg_find(m, name);
    char buf[32];
    char *end = NULL;
    long long v;

    if (out) {
        *out = def;
    }
    if (!kv || zms_kv_copy_value(kv, buf, sizeof(buf)) != 0 || buf[0] == '\0') {
        return -1;
    }
    v = strtoll(buf, &end, 10);
    if (!end || *end != '\0') {
        return -1;
    }
    if (out) {
        *out = (int64_t)v;
    }
    return 0;
}

int zms_msg_get_bool(const zms_msg *m, const char *name, int def)
{
    const zms_kv *kv = zms_msg_find(m, name);
    char buf[16];

    if (!kv || zms_kv_copy_value(kv, buf, sizeof(buf)) != 0) {
        return def;
    }
    if (str_ieq("1", buf, strlen(buf)) || str_ieq("true", buf, strlen(buf)) ||
        str_ieq("yes", buf, strlen(buf))) {
        return 1;
    }
    if (str_ieq("0", buf, strlen(buf)) || str_ieq("false", buf, strlen(buf)) ||
        str_ieq("no", buf, strlen(buf))) {
        return 0;
    }
    return def;
}

const char *zms_msg_query(const zms_msg *m, size_t *len)
{
    size_t i;

    if (len) {
        *len = 0;
    }
    if (!m || !m->url) {
        return NULL;
    }
    for (i = 0; i < m->url_len; ++i) {
        if (m->url[i] == '?') {
            if (len) {
                *len = m->url_len - i - 1;
            }
            return m->url + i + 1;
        }
    }
    return NULL;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int query_scan(const char *q, size_t qlen, const char *name, char *out, size_t cap)
{
    size_t nlen = strlen(name);
    size_t i = 0;

    while (i < qlen) {
        size_t key_start = i;
        size_t key_end;
        size_t val_start;
        size_t oi = 0;

        while (i < qlen && q[i] != '&') {
            ++i;
        }
        key_end = i;
        /* 找 '=' */
        {
            size_t eq = key_start;

            while (eq < key_end && q[eq] != '=') {
                ++eq;
            }
            val_start = (eq < key_end) ? eq + 1 : key_end;
            if (eq - key_start == nlen && strncmp(q + key_start, name, nlen) == 0) {
                for (size_t k = val_start; k < key_end && oi + 1 < cap; ++k) {
                    if (q[k] == '%' && k + 2 < key_end) {
                        int hi = hex_val(q[k + 1]);
                        int lo = hex_val(q[k + 2]);

                        if (hi >= 0 && lo >= 0) {
                            out[oi++] = (char)((hi << 4) | lo);
                            k += 2;
                            continue;
                        }
                    }
                    out[oi++] = (q[k] == '+') ? ' ' : q[k];
                }
                out[oi] = '\0';
                return 0;
            }
        }
        i = (i < qlen) ? i + 1 : i; /* 跳过 '&' */
    }
    if (cap) {
        out[0] = '\0';
    }
    return -1;
}

int zms_msg_query_get(const zms_msg *m, const char *name, char *out, size_t cap)
{
    size_t qlen = 0;
    const char *q;

    if (!out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    q = zms_msg_query(m, &qlen);
    if (!q || !name || !name[0]) {
        return -1;
    }
    return query_scan(q, qlen, name, out, cap);
}

int zms_msg_query_get_int(const zms_msg *m, const char *name, int64_t def, int64_t *out)
{
    char buf[32];
    char *end = NULL;
    long long v;

    if (out) {
        *out = def;
    }
    if (zms_msg_query_get(m, name, buf, sizeof(buf)) != 0 || buf[0] == '\0') {
        return -1;
    }
    v = strtoll(buf, &end, 10);
    if (!end || *end != '\0') {
        return -1;
    }
    if (out) {
        *out = (int64_t)v;
    }
    return 0;
}
