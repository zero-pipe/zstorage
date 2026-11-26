/**
 * @file gb_sip.c
 * @brief 极简 SIP 消息解析/构造 + Digest 认证（GB28181 固定消息集）。
 *
 * 只覆盖 GB28181 用到的 SIP 子集（REGISTER/INVITE/ACK/MESSAGE 及响应），
 * 不做完整 RFC3261：无事务状态机、无分支重传、无 SDP 内嵌处理。所有输入
 * 都被当成“不可信文本”做长度钳制与转义，避免解析越界。
 *
 * 无共享状态：解析函数把结果写进调用方给的 gb_sip_msg，构造函数写进调用方
 * 缓冲；除 gb_rand_hex 用了进程级静态计数器外，均可在任意线程调用。
 */
#include "device/gb28181/gb28181_internal.h"
#include "zms/util/md5.h"
#include "zn/platform.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <winsock2.h>
#if defined(_MSC_VER)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
#else
#include <arpa/inet.h>
#endif

/* ── 字符串缓冲 ─────────────────────────────────────────────────────────── */

/* 所有 gb_sb_* 都是“写满即停”的定长追加：绝不越界，也不报错。SIP 报文本身
 * 有大小上限（GB_SIP_MSG_MAX），构建时宁可静默截断，调用方用返回值长度判断
 * 是否塞得下。 */

void gb_sb_init(gb_sb *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    if (cap) {
        buf[0] = '\0';
    }
}

void gb_sb_puts(gb_sb *w, const char *s)
{
    while (s && *s) {
        if (w->len + 1 < w->cap) {
            w->buf[w->len++] = *s;
            w->buf[w->len] = '\0';
        }
        ++s;
    }
}

void gb_sb_printf(gb_sb *w, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (w->len + 1 >= w->cap) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n >= w->cap - w->len) {
        w->len = w->cap - 1;
    } else {
        w->len += (size_t)n;
    }
}

void gb_sb_xml_esc(gb_sb *w, const char *s)
{
    /* XML 特殊字符转义：GB28181 的 MANSCDP/PTZ 消息体是 XML，设备 ID、名称
     * 里若含 & < > " 不转义会破坏报文结构。 */
    while (s && *s) {
        switch (*s) {
        case '&':
            gb_sb_puts(w, "&amp;");
            break;
        case '<':
            gb_sb_puts(w, "&lt;");
            break;
        case '>':
            gb_sb_puts(w, "&gt;");
            break;
        case '"':
            gb_sb_puts(w, "&quot;");
            break;
        default:
            if (w->len + 1 < w->cap) {
                w->buf[w->len++] = *s;
                w->buf[w->len] = '\0';
            }
            break;
        }
        ++s;
    }
}

void gb_rand_hex(char *out, size_t digits)
{
    static unsigned counter;
    unsigned seed;

    if (!out || digits == 0) {
        return;
    }
    /* 这里生成的是 SIP tag/branch/cnonce 这类“需要唯一但不需密码学安全”的
     * 标识。每次调用都 srand（而非初始化一次）是为了让连续两次调用也能拿到
     * 不同序列：把单调时钟、自增计数、输出地址异或后做种子，抵消 rand()
     * 初始种子过近导致的重复。 */
    seed = (unsigned)zn_monotonic_ms() ^ (++counter * 2654435761u) ^ (unsigned)(size_t)out;
    srand(seed);
    for (size_t i = 0; i < digits; ++i) {
        static const char hex[] = "0123456789abcdef";
        out[i] = hex[rand() & 0x0F];
    }
    out[digits] = '\0';
}

/* ── 解析 ───────────────────────────────────────────────────────────────── */

static const char *compact_name(const char *name)
{
    /* SIP 允许单字母压缩头名（v/f/t/i/m/l/c/s）。把它们统一展开成全名，
     * 后续查找无论用全名还是压缩名都能命中；本来就非压缩名的原样返回。 */
    if (strcmp(name, "v") == 0) {
        return "Via";
    }
    if (strcmp(name, "f") == 0) {
        return "From";
    }
    if (strcmp(name, "t") == 0) {
        return "To";
    }
    if (strcmp(name, "i") == 0) {
        return "Call-ID";
    }
    if (strcmp(name, "m") == 0) {
        return "Contact";
    }
    if (strcmp(name, "l") == 0) {
        return "Content-Length";
    }
    if (strcmp(name, "c") == 0) {
        return "Content-Type";
    }
    if (strcmp(name, "s") == 0) {
        return "Subject";
    }
    return name;
}

static char *trim(char *s)
{
    char *end;

    /* 去首尾的空白与 CR/LF（后者的存在是因为按行切分时可能保留了行尾 \r）。
     * 原地修改并返回新的起始指针。 */
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        ++s;
    }
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

char *gb_sip_tx_alloc(void)
{
    /* 统一按 GB_SIP_MSG_MAX 分配发送缓冲：调用方拿到的保证能容纳任意一条
     * 合法 GB28181 消息，构建函数返回的长度即实际使用量。 */
    return (char *)malloc(GB_SIP_MSG_MAX);
}

const char *gb_sip_hdr(const gb_sip_msg *m, const char *name)
{
    int i;

    if (!m || !name) {
        return NULL;
    }
    /* 头名大小写不敏感（RFC3261 规定）。返回的是 msg 内部存储的指针，
     * life-time 与本 msg 相同，调用方不要长期持有。 */
    for (i = 0; i < m->hdr_count; ++i) {
        if (strcasecmp(m->hdrs[i].name, name) == 0) {
            return m->hdrs[i].value;
        }
    }
    return NULL;
}

static void hdr_add(gb_sip_msg *m, const char *name, const char *value)
{
    /* 头数量到上限就静默丢弃多余头：解析阶段不能因为畸形大报文而崩，
     * GB_SIP_HDR_MAX 足够覆盖真实消息。 */
    if (m->hdr_count >= GB_SIP_HDR_MAX) {
        return;
    }
    snprintf(m->hdrs[m->hdr_count].name, sizeof(m->hdrs[m->hdr_count].name), "%s", name);
    snprintf(m->hdrs[m->hdr_count].value, sizeof(m->hdrs[m->hdr_count].value), "%s", value);
    ++m->hdr_count;
}

int gb_sip_parse(const void *data, size_t len, const char *peer_ip, uint16_t peer_port,
                 gb_sip_msg *out)
{
    const char *p = (const char *)data;
    const char *end = p + len;
    const char *hdr_end;
    char line[GB_SIP_LINE_MAX];
    int first = 1;
    size_t clen = 0;
    int have_clen = 0;

    if (!data || !out || len == 0) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (peer_ip) {
        snprintf(out->peer_ip, sizeof(out->peer_ip), "%s", peer_ip);
    }
    out->peer_port = peer_port;

    /* header/body 分界 */
    /* 找空行（CRLFCRLF 或 LFLF）。找不到就当整段都是头，
     * 这样畸形报文也不会把后面的 body 误当头部行解析。 */
    hdr_end = NULL;
    for (const char *q = p; q + 3 < end; ++q) {
        if (q[0] == '\r' && q[1] == '\n' && q[2] == '\r' && q[3] == '\n') {
            hdr_end = q;
            break;
        }
        if (q[0] == '\n' && q[1] == '\n') {
            hdr_end = q;
            break;
        }
    }
    if (!hdr_end) {
        hdr_end = end;
    }

    while (p < hdr_end) {
        const char *eol = memchr(p, '\n', (size_t)(hdr_end - p));
        size_t n;
        char *s;

        if (!eol) {
            eol = hdr_end;
        }
        n = (size_t)(eol - p);
        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p = eol < hdr_end ? eol + 1 : eol;

        s = trim(line);
        if (*s == '\0') {
            break;
        }
        if (first) {
            /* 首行二选一：以 "SIP/2.0" 开头是响应（取状态码+原因短语），
             * 否则是请求（取方法 + URI）。解析失败返回 -1，让上层丢弃。 */
            first = 0;
            if (strncmp(s, "SIP/2.0", 7) == 0) {
                char *sp = strchr(s + 7, ' ');
                out->is_response = 1;
                out->status = sp ? atoi(sp + 1) : 0;
                if (sp) {
                    char *r = strchr(sp + 1, ' ');
                    snprintf(out->reason, sizeof(out->reason), "%s", r ? r + 1 : "");
                }
            } else {
                char *sp = strchr(s, ' ');
                if (!sp) {
                    return -1;
                }
                *sp = '\0';
                snprintf(out->method, sizeof(out->method), "%s", s);
                s = trim(sp + 1);
                sp = strchr(s, ' ');
                if (sp) {
                    *sp = '\0';
                }
                snprintf(out->uri, sizeof(out->uri), "%s", s);
            }
            continue;
        }
        {
            char *colon = strchr(s, ':');
            char name[40];
            char *value;

            if (!colon) {
                continue;
            }
            *colon = '\0';
            value = trim(colon + 1);
            snprintf(name, sizeof(name), "%s", trim(s));
            if (name[0] == ' ' || name[0] == '\t') {
                continue;
            }
            /* 折行续行（简化：忽略，但保留最后一段） */
            {
                const char *canon = compact_name(name);
                hdr_add(out, canon, value);
            }
            if (strcasecmp(compact_name(name), "Content-Length") == 0) {
                clen = (size_t)strtoul(value, NULL, 10);
                have_clen = 1;
            }
        }
    }

    {
        const char *body = hdr_end;
        size_t avail;
        size_t take;

        if (body + 3 < end && body[0] == '\r' && body[1] == '\n' && body[2] == '\r' &&
            body[3] == '\n') {
            body += 4;
        } else if (body + 1 < end && body[0] == '\n' && body[1] == '\n') {
            body += 2;
        } else if (body < end && body[0] == '\r') {
            body += 2;
        } else if (body < end && body[0] == '\n') {
            body += 1;
        }
        if (body > end) {
            body = end;
        }
        avail = (size_t)(end - body);
        /* 有 Content-Length 且比实际到达的字节少 → 只取声明的长度（防止把
         * 后续拼接进来的下一个报文吃进 body）；否则信任实际长度直到缓冲尾。 */
        take = have_clen && clen < avail ? clen : avail;
        if (take > sizeof(out->body) - 1) {
            take = sizeof(out->body) - 1;
        }
        if (take) {
            memcpy(out->body, body, take);
        }
        out->body[take] = '\0';
        out->body_len = take;
    }
    return 0;
}

void gb_sip_uri_user(const char *uri, char *out, size_t cap)
{
    const char *p;
    const char *at;
    const char *stop;

    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!uri) {
        return;
    }
    /* 从 "sip:user@host" / "<sip:user@host>" / 带 "://" 的 URL 里取 user 部分：
     * 跳过 scheme 与前导 '<'，取 '@' 之前的子串（无 '@' 则以整段为 user）。 */
    p = uri;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '<') {
        ++p;
    }
    if (strncasecmp(p, "sips:", 5) == 0) {
        p += 5;
    } else if (strncasecmp(p, "sip:", 4) == 0) {
        p += 4;
    } else if (strstr(p, "://")) {
        p = strstr(p, "://") + 3;
    }
    at = strchr(p, '@');
    stop = at ? at : p + strlen(p);
    {
        size_t n = (size_t)(stop - p);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, p, n);
        out[n] = '\0';
    }
}

void gb_sip_uri_host(const char *uri, char *out, size_t cap)
{
    const char *p;
    const char *at;
    const char *stop;

    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!uri) {
        return;
    }
    p = uri;
    if (strncasecmp(p, "sips:", 5) == 0) {
        p += 5;
    } else if (strncasecmp(p, "sip:", 4) == 0) {
        p += 4;
    } else if (strstr(p, "://")) {
        p = strstr(p, "://") + 3;
    }
    at = strchr(p, '@');
    if (at) {
        p = at + 1;
    }
    stop = p + strcspn(p, ";> \t\r\n");
    {
        size_t n = (size_t)(stop - p);
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(out, p, n);
        out[n] = '\0';
    }
}

void gb_sip_param(const char *value, const char *name, char *out, size_t cap)
{
    const char *p;
    size_t nlen;

    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    if (!value || !name) {
        return;
    }
    /* 从一组 "a=b" 里按 ';' 分隔取参数：支持 "k=v" 与 "k=\"quoted v\"" 两种
     * 写法（后者值里可含 ';'）。name 是大小写不敏感的键名。 */
    nlen = strlen(name);
    p = value;
    while ((p = strchr(p, ';')) != NULL) {
        const char *k;
        const char *v;

        ++p;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (strncasecmp(p, name, nlen) != 0) {
            continue;
        }
        k = p + nlen;
        while (*k == ' ' || *k == '\t') {
            ++k;
        }
        if (*k != '=') {
            continue;
        }
        ++k;
        while (*k == ' ' || *k == '\t') {
            ++k;
        }
        if (*k == '"') {
            ++k;
            v = k;
            while (*v && *v != '"') {
                ++v;
            }
        } else {
            v = k;
            while (*v && *v != ';' && *v != ' ' && *v != '\t' && *v != '\r' && *v != '\n') {
                ++v;
            }
        }
        {
            size_t n = (size_t)(v - k);
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, k, n);
            out[n] = '\0';
        }
        return;
    }
}

/* ── 构造 ───────────────────────────────────────────────────────────────── */

int gb_sip_build_request(char *out, size_t cap, const gb_sip_req_spec *s)
{
    gb_sb w;
    char branch[64];
    char cseq_method[16];
    const char *tr;

    if (!out || !s || !s->method || !s->uri) {
        return -1;
    }
    tr = s->transport && s->transport[0] ? s->transport : "UDP";
    gb_sb_init(&w, out, cap);
    gb_rand_hex(branch, 20);

    snprintf(cseq_method, sizeof(cseq_method), "%s", s->method);
    gb_sb_printf(&w, "%s %s SIP/2.0\r\n", s->method, s->uri);
    /* Via：rport 让对端回包时把真实源端口填进 received/rport（穿透 NAT）；
     * branch 用 "z9hG4bK" 魔数前缀 + 随机，符合 RFC3261 的事务标识要求。 */
    if (s->local_ip && s->local_ip[0]) {
        gb_sb_printf(&w, "Via: SIP/2.0/%s %s:%u;rport;branch=z9hG4bK%s\r\n", tr,
                     s->local_ip, s->local_port, branch);
    } else {
        gb_sb_printf(&w, "Via: SIP/2.0/%s %u;rport;branch=z9hG4bK%s\r\n",
                     tr, s->local_port, branch);
    }
    gb_sb_printf(&w, "From: <sip:%s@%s>;tag=%s\r\n", s->from_user ? s->from_user : "",
                 s->from_host ? s->from_host : "", s->from_tag ? s->from_tag : "");
    gb_sb_printf(&w, "To: <sip:%s@%s>\r\n", s->to_user ? s->to_user : "",
                 s->to_host ? s->to_host : "");
    gb_sb_printf(&w, "Call-ID: %s\r\n", s->call_id ? s->call_id : "");
    gb_sb_printf(&w, "CSeq: %u %s\r\n", s->cseq, cseq_method);
    /* Max-Forwards=70 是 SIP 请求的惯用初值，每经过一跳递减，防止环路。 */
    gb_sb_puts(&w, "Max-Forwards: 70\r\n");
    gb_sb_puts(&w, "User-Agent: ZMS-GB28181/0.1\r\n");
    if (s->local_ip && s->local_ip[0]) {
        if (tr[0] == 'T' || tr[0] == 't') {
            gb_sb_printf(&w, "Contact: <sip:%s@%s:%u;transport=%s>\r\n",
                         s->contact_user ? s->contact_user : s->from_user,
                         s->local_ip, s->local_port,
                         tr[0] == 'T' ? "tcp" : "tls");
        } else {
            gb_sb_printf(&w, "Contact: <sip:%s@%s:%u>\r\n",
                         s->contact_user ? s->contact_user : s->from_user,
                         s->local_ip, s->local_port);
        }
    }
    if (s->has_expires) {
        gb_sb_printf(&w, "Expires: %d\r\n", s->expires);
    }
    if (s->subject) {
        gb_sb_printf(&w, "Subject: %s\r\n", s->subject);
    }
    if (s->authorization) {
        gb_sb_printf(&w, "Authorization: %s\r\n", s->authorization);
    }
    if (s->content_type) {
        gb_sb_printf(&w, "Content-Type: %s\r\n", s->content_type);
    }
    /* Content-Length 必须最后写、且与 body 实际字节一致；空 body 时为 0。
     * 后面的 body 直接 memcpy 到缓冲，用 w.len 回填返回值。 */
    gb_sb_printf(&w, "Content-Length: %u\r\n\r\n", (unsigned)s->body_len);
    if (s->body_len) {
        gb_sb_puts(&w, "");
        if (w.len + s->body_len + 1 > w.cap) {
            return -1;
        }
        memcpy(w.buf + w.len, s->body, s->body_len);
        w.len += s->body_len;
        w.buf[w.len] = '\0';
    }
    return (int)w.len;
}

int gb_sip_build_response_ex(char *out, size_t cap, const gb_sip_msg *req, int status,
                             const char *reason, const char *local_ip, unsigned local_port,
                             const char *to_tag, const char *contact_user,
                             const char *content_type, const void *body, size_t body_len,
                             const char *extra_headers)
{
    gb_sb w;
    const char *v;
    int i;

    if (!out || !req) {
        return -1;
    }
    gb_sb_init(&w, out, cap);
    gb_sb_printf(&w, "SIP/2.0 %d %s\r\n", status, reason ? reason : "OK");

    /* 响应必须回显请求里的关键头（Via/From/To/Call-ID/CSeq），对端靠这些做
     * 事务匹配。Via 可能有多条，逐条回显。To 头首次响应要补 tag（对话标识），
     * 但若请求里已带 tag 则保留原样。 */
    for (i = 0; i < req->hdr_count; ++i) {
        if (strcasecmp(req->hdrs[i].name, "Via") == 0) {
            gb_sb_printf(&w, "Via: %s\r\n", req->hdrs[i].value);
        }
    }
    if ((v = gb_sip_hdr(req, "From")) != NULL) {
        gb_sb_printf(&w, "From: %s\r\n", v);
    }
    if ((v = gb_sip_hdr(req, "To")) != NULL) {
        /* 首次响应补 To tag（对话的本地标识）；若请求已带 tag（对话内请求）
         * 则不再追加，避免出现两个 tag。 */
        if (to_tag && to_tag[0] && !strstr(v, "tag=")) {
            gb_sb_printf(&w, "To: %s;tag=%s\r\n", v, to_tag);
        } else {
            gb_sb_printf(&w, "To: %s\r\n", v);
        }
    }
    if ((v = gb_sip_hdr(req, "Call-ID")) != NULL) {
        gb_sb_printf(&w, "Call-ID: %s\r\n", v);
    }
    if ((v = gb_sip_hdr(req, "CSeq")) != NULL) {
        gb_sb_printf(&w, "CSeq: %s\r\n", v);
    }
    gb_sb_puts(&w, "User-Agent: ZMS-GB28181/0.1\r\n");
    if (contact_user && local_ip && local_ip[0]) {
        /* Contact 告诉对端本端信令可达地址（后续 INVITE/ACK 用）。仅有本地
         * IP 时才写，避免给出无效地址。 */
        gb_sb_printf(&w, "Contact: <sip:%s@%s:%u>\r\n", contact_user, local_ip, local_port);
    }
    if (extra_headers && extra_headers[0]) {
        gb_sb_puts(&w, extra_headers);
    }
    if (content_type && body_len) {
        gb_sb_printf(&w, "Content-Type: %s\r\n", content_type);
    }
    gb_sb_printf(&w, "Content-Length: %u\r\n\r\n", (unsigned)body_len);
    if (body_len && body) {
        if (w.len + body_len + 1 > w.cap) {
            return -1;
        }
        memcpy(w.buf + w.len, body, body_len);
        w.len += body_len;
        w.buf[w.len] = '\0';
    }
    (void)local_port;
    return (int)w.len;
}

int gb_sip_build_response(char *out, size_t cap, const gb_sip_msg *req, int status,
                          const char *reason, const char *local_ip, unsigned local_port,
                          const char *to_tag, const char *contact_user, const char *content_type,
                          const void *body, size_t body_len)
{
    return gb_sip_build_response_ex(out, cap, req, status, reason, local_ip, local_port, to_tag,
                                    contact_user, content_type, body, body_len, NULL);
}

/* ── Digest ─────────────────────────────────────────────────────────────── */

static int auth_param(const char *s, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    const char *p = s;

    /* 在 "key=value, key2=\"...\"" 形式的 Authorization/WWW-Authenticate 里
     * 找指定 key。要求 key 左侧是分隔符（空格/逗号/制表/引号）以避免把
     * "nonce" 误匹配到 "cnonce" 之类的前缀子串。值可为 quoted 或裸词。 */
    while ((p = strstr(p, key)) != NULL) {
        const char *q = p + klen;

        if (p != s && p[-1] != ' ' && p[-1] != ',' && p[-1] != '\t' && p[-1] != '"') {
            p = q;
            continue;
        }
        while (*q == ' ' || *q == '\t') {
            ++q;
        }
        if (*q != '=') {
            p = q;
            continue;
        }
        ++q;
        while (*q == ' ' || *q == '\t') {
            ++q;
        }
        if (*q == '"') {
            ++q;
            {
                const char *e = strchr(q, '"');
                size_t n = e ? (size_t)(e - q) : strlen(q);
                if (n >= cap) {
                    n = cap - 1;
                }
                memcpy(out, q, n);
                out[n] = '\0';
            }
        } else {
            size_t n = strcspn(q, ", \t\r\n");
            if (n >= cap) {
                n = cap - 1;
            }
            memcpy(out, q, n);
            out[n] = '\0';
        }
        return 1;
    }
    return 0;
}

int gb_digest_parse_challenge(const char *www_auth, gb_digest_challenge *out)
{
    if (!www_auth || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    /* realm 与 nonce 是 Digest 认证的必需项，缺任一即失败；opaque/qop/
     * algorithm 缺失可接受（很多设备不发 opaque）。 */
    if (!auth_param(www_auth, "realm", out->realm, sizeof(out->realm))) {
        return -1;
    }
    auth_param(www_auth, "nonce", out->nonce, sizeof(out->nonce));
    auth_param(www_auth, "opaque", out->opaque, sizeof(out->opaque));
    auth_param(www_auth, "qop", out->qop, sizeof(out->qop));
    auth_param(www_auth, "algorithm", out->algorithm, sizeof(out->algorithm));
    return out->nonce[0] ? 0 : -1;
}

int gb_digest_verify(const char *auth_header, const char *method, const char *uri,
                     const char *realm, const char *nonce, const char *user, const char *pass)
{
    char r_realm[128] = "";
    char r_nonce[128] = "";
    char r_uri[256] = "";
    char r_user[128] = "";
    char r_resp[64] = "";
    char qop[32] = "";
    char nc[32] = "";
    char cnonce[64] = "";
    char ha1[33];
    char ha2[33];
    char kd[512];
    char expected[33];
    char src[512];

    /* 校验顺序：先用 Authorization 头里自带的 realm/nonce/uri/user 覆盖入参
     * （对端可能用不同值），再按 RFC2617 算 HA1/HA2/response。带 qop=auth 时
     * 响应公式多 nc 与 cnonce 两段。任何必需参数缺失都返回 0（认证失败）。 */
    if (!auth_header || !method || !uri || !realm || !nonce || !user || !pass) {
        return 0;
    }
    if (!auth_param(auth_header, "response", r_resp, sizeof(r_resp)) || !r_resp[0]) {
        return 0;
    }
    (void)auth_param(auth_header, "realm", r_realm, sizeof(r_realm));
    (void)auth_param(auth_header, "nonce", r_nonce, sizeof(r_nonce));
    (void)auth_param(auth_header, "uri", r_uri, sizeof(r_uri));
    (void)auth_param(auth_header, "username", r_user, sizeof(r_user));
    (void)auth_param(auth_header, "qop", qop, sizeof(qop));
    (void)auth_param(auth_header, "nc", nc, sizeof(nc));
    (void)auth_param(auth_header, "cnonce", cnonce, sizeof(cnonce));

    if (r_realm[0]) {
        realm = r_realm;
    }
    if (r_nonce[0]) {
        nonce = r_nonce;
    }
    if (r_uri[0]) {
        uri = r_uri;
    }
    if (r_user[0]) {
        user = r_user;
    }
    snprintf(src, sizeof(src), "%s:%s:%s", user, realm, pass);
    zms_md5_hex_str(src, ha1);
    snprintf(src, sizeof(src), "%s:%s", method, uri);
    zms_md5_hex_str(src, ha2);
    if (qop[0] && strstr(qop, "auth") != NULL) {
        snprintf(kd, sizeof(kd), "%s:%s:%s:%s:%s:%s", ha1, nonce, nc[0] ? nc : "00000001",
                 cnonce[0] ? cnonce : "", "auth", ha2);
    } else {
        snprintf(kd, sizeof(kd), "%s:%s:%s", ha1, nonce, ha2);
    }
    zms_md5_hex_str(kd, expected);
    return strcasecmp(expected, r_resp) == 0;
}

int gb_digest_build_authorization(const char *user, const char *pass, const char *method,
                                  const char *uri, const gb_digest_challenge *ch, int nc,
                                  char *out, size_t cap)
{
    char ha1[33];
    char ha2[33];
    char kd[256];
    char src[512];
    char response[33];
    char cnonce[33];
    int use_qop;

    if (!user || !pass || !method || !uri || !ch || !out || cap == 0) {
        return -1;
    }
    snprintf(src, sizeof(src), "%s:%s:%s", user, ch->realm, pass);
    zms_md5_hex_str(src, ha1);
    snprintf(src, sizeof(src), "%s:%s", method, uri);
    zms_md5_hex_str(src, ha2);

    use_qop = ch->qop[0] && strstr(ch->qop, "auth") != NULL;
    /* 服务器支持 auth qop 时，响应里必须带 nc（8 位十六进制，每请求递增）和
     * cnonce（本次随机），并参与 response 计算，否则对端校验不过。 */
    if (use_qop) {
        gb_rand_hex(cnonce, 16);
        snprintf(kd, sizeof(kd), "%s:%s:%08x:%s:%s:%s", ha1, ch->nonce, nc, cnonce, "auth", ha2);
    } else {
        snprintf(kd, sizeof(kd), "%s:%s:%s", ha1, ch->nonce, ha2);
    }
    zms_md5_hex_str(kd, response);

    if (use_qop) {
        snprintf(out, cap,
                 "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", algorithm=MD5, qop=auth, nc=%08x, cnonce=\"%s\"",
                 user, ch->realm, ch->nonce, uri, response, nc, cnonce);
    } else {
        snprintf(out, cap,
                 "Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                 "response=\"%s\", algorithm=MD5",
                 user, ch->realm, ch->nonce, uri, response);
    }
    return 0;
}
