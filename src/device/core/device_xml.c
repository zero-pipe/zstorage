/**
 * @file device_xml.c
 * @brief 设备协议共用 XML 辅助实现。
 */
#include "zcs/device/core/device_xml.h"

#include <ctype.h>
#include <string.h>

static int ascii_ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

/** p 指向 '<' 之后；判断是否为本地名 name 的元素（忽略命名空间前缀）。 */
static int tag_is(const char *p, const char *name, size_t name_len)
{
    const char *q;
    const char *local = p;

    if (!p || !*p || *p == '/' || *p == '?' || *p == '!') {
        return 0;
    }
    for (q = p; *q; ++q) {
        if (*q == '>' || *q == '/' || *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
            break;
        }
        if (*q == ':') {
            local = q + 1;
        }
    }
    return (size_t)(q - local) == name_len && ascii_ieq_n(local, name, name_len);
}

const char *zcs_xml_find_between(const char *from, const char *end, const char *name)
{
    size_t name_len;
    const char *p;

    if (!from || !name) {
        return NULL;
    }
    name_len = strlen(name);
    p = from;
    while ((p = strchr(p, '<')) != NULL) {
        const char *gt;
        if (end && p >= end) {
            return NULL;
        }
        if (tag_is(p + 1, name, name_len)) {
            gt = strchr(p, '>');
            return gt ? gt + 1 : NULL;
        }
        ++p;
    }
    return NULL;
}

const char *zcs_xml_find_tag_start(const char *from, const char *end, const char *name)
{
    size_t name_len;
    const char *p;

    if (!from || !name) {
        return NULL;
    }
    name_len = strlen(name);
    p = from;
    while ((p = strchr(p, '<')) != NULL) {
        if (end && p >= end) {
            return NULL;
        }
        if (tag_is(p + 1, name, name_len)) {
            return p;
        }
        ++p;
    }
    return NULL;
}

const char *zcs_xml_find(const char *from, const char *name)
{
    return zcs_xml_find_between(from, NULL, name);
}

static void xml_unescape(char *s)
{
    char *w = s;
    const char *r = s;

    while (*r) {
        if (*r == '&') {
            if (strncmp(r, "&amp;", 5) == 0) {
                *w++ = '&';
                r += 5;
                continue;
            }
            if (strncmp(r, "&lt;", 4) == 0) {
                *w++ = '<';
                r += 4;
                continue;
            }
            if (strncmp(r, "&gt;", 4) == 0) {
                *w++ = '>';
                r += 4;
                continue;
            }
            if (strncmp(r, "&quot;", 6) == 0) {
                *w++ = '"';
                r += 6;
                continue;
            }
            if (strncmp(r, "&apos;", 6) == 0) {
                *w++ = '\'';
                r += 6;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

int zcs_xml_text_between(const char *from, const char *end, const char *name, char *out,
                         size_t cap)
{
    const char *t;
    const char *lt;
    size_t len;

    if (!out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    t = zcs_xml_find_between(from, end, name);
    if (!t || (end && t >= end)) {
        return -1;
    }
    lt = strchr(t, '<');
    if (!lt) {
        return -1;
    }
    len = (size_t)(lt - t);
    while (len > 0 && isspace((unsigned char)*t)) {
        ++t;
        --len;
    }
    while (len > 0 && isspace((unsigned char)t[len - 1])) {
        --len;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(out, t, len);
    out[len] = '\0';
    xml_unescape(out);
    return 0;
}

int zcs_xml_text(const char *from, const char *name, char *out, size_t cap)
{
    return zcs_xml_text_between(from, NULL, name, out, cap);
}

int zcs_xml_attr(const char *from, const char *name, const char *attr, char *out, size_t cap)
{
    size_t name_len;
    size_t attr_len;
    const char *p;
    const char *gt;
    const char *q;

    if (!from || !name || !attr || !out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    name_len = strlen(name);
    attr_len = strlen(attr);
    p = from;
    while ((p = strchr(p, '<')) != NULL) {
        if (tag_is(p + 1, name, name_len)) {
            break;
        }
        ++p;
    }
    if (!p) {
        return -1;
    }
    gt = strchr(p, '>');
    if (!gt) {
        return -1;
    }
    q = p;
    while ((q = strstr(q, attr)) != NULL && q < gt) {
        const char *v;
        if (q > p && !isspace((unsigned char)q[-1])) {
            ++q;
            continue;
        }
        v = q + attr_len;
        while (v < gt && isspace((unsigned char)*v)) {
            ++v;
        }
        if (v >= gt || *v != '=') {
            ++q;
            continue;
        }
        ++v;
        while (v < gt && isspace((unsigned char)*v)) {
            ++v;
        }
        if (v < gt && (*v == '"' || *v == '\'')) {
            char quote = *v++;
            const char *e = strchr(v, quote);
            size_t n = e && e < gt ? (size_t)(e - v) : 0;
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            xml_unescape(out);
            return 0;
        }
        {
            size_t n = (size_t)(gt - v);
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, v, n);
            out[n] = '\0';
            return 0;
        }
    }
    return -1;
}

int zcs_xml_local_name(const char *tag_start, char *out, size_t cap)
{
    const char *p;
    const char *local;
    const char *q;

    if (!tag_start || !out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    p = tag_start;
    if (*p == '<') {
        ++p;
    }
    if (*p == '/' || *p == '?' || *p == '!' || !*p) {
        return -1;
    }
    local = p;
    for (q = p; *q; ++q) {
        if (*q == '>' || *q == '/' || *q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
            break;
        }
        if (*q == ':') {
            local = q + 1;
        }
    }
    {
        size_t n = (size_t)(q - local);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, local, n);
        out[n] = '\0';
    }
    return out[0] ? 0 : -1;
}

int zcs_xml_first_child_name(const char *xml, const char *parent, char *out, size_t cap)
{
    const char *p;
    const char *gt;

    if (!xml || !parent || !out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    p = zcs_xml_find_tag_start(xml, NULL, parent);
    if (!p) {
        return -1;
    }
    gt = strchr(p, '>');
    if (!gt) {
        return -1;
    }
    p = gt + 1;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != '<') {
        return -1;
    }
    return zcs_xml_local_name(p, out, cap);
}
