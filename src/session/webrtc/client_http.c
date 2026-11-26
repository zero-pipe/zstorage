/**
 * @file client_http.c
 * @brief WebRTC client 用最小 HTTP(S) POST。
 *
 * 为什么自造而不用现成的 HTTP 客户端：这个请求只服务于 WebRTC 信令
 * （POST SDP 到 WHIP/信令端点，读到 201 + Location 即可），只需要单次
 * 请求-响应、无需连接复用/重定向跟随。自己写能把整个生命周期挂在调用方
 * 的 poller 上做非阻塞，避免在信令线程里同步阻塞 socket。
 *
 * 并发：所有回调（on_data/on_timeout/on_error）都在传入的 poller 线程上
 * 触发；zms_http_client 的字段无需加锁，因为创建、回调、销毁都串行于该
 * poller。唯一的跨线程约定是 cb 只会被调用一次，见 http_finish。
 */
#include "session/webrtc/client_http.h"
#include "zms/util/dns_cache.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tls_client.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define strncasecmp _strnicmp
#endif

#define ZMS_HTTP_RX_MAX (256u * 1024u)
#define ZMS_HTTP_BODY_MAX (128u * 1024u)

struct zms_http_client {
    zn_poller *poller;
    zn_poller_timer *timer;
    zn_tcp_client *tcp;
    zn_tls_client *tls;
    char host[128];
    uint16_t port;
    char path[768];
    int https;
    char *req;
    size_t req_len;
    char *rx;
    size_t rx_len;
    char *body;
    size_t body_len;
    size_t chunk_off;
    int body_alloc;
    int chunked;
    int chunk_done;
    long content_length;
    size_t body_off;
    int status;
    char location[512];
    int done;
    zms_http_client_cb cb;
    void *user;
};

static void http_finish_async(void *user);

static void http_capture_location(zms_http_client *c, const char *line, size_t len)
{
    const char *v;
    size_t i;

    if (!c || len < 10) {
        return;
    }
    if (strncasecmp(line, "Location:", 9) != 0) {
        return;
    }
    v = line + 9;
    while (*v == ' ' || *v == '\t') {
        ++v;
    }
    for (i = 0; i < sizeof(c->location) - 1 && (size_t)(v - line) + i < len && v[i] != '\r' &&
                v[i] != '\n';
         ++i) {
        c->location[i] = v[i];
    }
    c->location[i] = '\0';
}

/* 解析响应头。返回 1=头部已完整解析（body_off 已定位），
 * 0=数据还不够（等下一个 on_data），-1=格式错误。
 *
 * 只关心三样：状态行、Location（WHIP 用它返回资源地址）、以及
 * Content-Length / Transfer-Encoding 用来决定 body 读取方式。 */
static int http_parse_headers(zms_http_client *c)
{
    char *end;
    char *line;
    char *hdr_end;
    char *p;

    end = strstr(c->rx, "\r\n\r\n");
    if (!end) {
        return 0;
    }
    hdr_end = end + 4;
    c->body_off = (size_t)(hdr_end - c->rx);
    line = strchr(c->rx, '\r');
    if (!line) {
        return -1;
    }
    if (sscanf(c->rx, "HTTP/%*s %d", &c->status) != 1) {
        return -1;
    }
    p = strstr(c->rx, "\r\n");
    if (!p) {
        return 0;
    }
    p += 2;
    while (p && p < hdr_end) {
        char *next = strstr(p, "\r\n");
        size_t len;

        if (!next || next > hdr_end) {
            break;
        }
        len = (size_t)(next - p);
        if (len == 0) {
            break;
        }
        http_capture_location(c, p, len);
        if (strncasecmp(p, "Content-Length:", 15) == 0) {
            c->content_length = strtol(p + 15, NULL, 10);
        } else if (strncasecmp(p, "Transfer-Encoding:", 18) == 0 && strstr(p, "chunked")) {
            c->chunked = 1;
        }
        p = next + 2;
    }
    zn_info("[webrtc-http] response status=%d location=%s clen=%ld chunked=%d", c->status,
             c->location[0] ? c->location : "-", c->content_length, c->chunked);
    return 1;
}

/* @return 1=完成，0=继续等待，-1=错误 */
/*
 * 两种 body 读取方式：
 *   - Content-Length：body 就在 rx 缓冲里，直接指向它（零拷贝）。
 *   - chunked：长度未知且分块，无法原地指向，必须另开 body 缓冲边解边拼。
 * 这就是 chunked 分支要 malloc 的原因。 */
static int http_parse_body(zms_http_client *c)
{
    if (!c->chunked) {
        if (c->content_length >= 0) {
            size_t need = c->body_off + (size_t)c->content_length;
            if (c->rx_len < need) {
                return 0;
            }
            c->body = c->rx + c->body_off;
            c->body_len = (size_t)c->content_length;
            return 1;
        }
        return 0;
    }
    if (c->chunk_done) {
        return 1;
    }
    if (!c->body) {
        c->body = (char *)malloc(ZMS_HTTP_BODY_MAX);
        if (!c->body) {
            return -1;
        }
        c->body_alloc = 1;
    }
    for (;;) {
        char *start = c->rx + c->body_off + c->chunk_off;
        char *crlf = strstr(start, "\r\n");
        /* avail 必须做差前先判大小：body_off+chunk_off 可能因起始 offset
         * 尚未到 rx_len 而越界，直接相减会得到巨大的 size_t。 */
        size_t avail = c->body_off + c->chunk_off <= c->rx_len
                           ? c->rx_len - c->body_off - c->chunk_off
                           : 0;
        size_t size;
        char *endp;

        if (!crlf) {
            return 0;
        }
        size = (size_t)strtoul(start, &endp, 16);
        c->chunk_off += (size_t)(crlf + 2 - start);
        if (size == 0) {
            /* 结束块：跳过结尾 CRLF（可能还有 trailers）。 */
            if (c->rx_len >= c->body_off + c->chunk_off + 2) {
                c->chunk_done = 1;
                return 1;
            }
            return 0;
        }
        if (avail < (size_t)(crlf + 2 - start) + size + 2) {
            return 0;
        }
        if (c->body_len + size > ZMS_HTTP_BODY_MAX) {
            return -1;
        }
        memcpy(c->body + c->body_len, crlf + 2, size);
        c->body_len += size;
        c->chunk_off += size + 2;
    }
}

/* 真正回调并销毁客户端的地方。
 *
 * 为什么不直接在触发点上 cb+destroy：触发点往往还在 tcp/tls 的 on_recv
 * 回调栈里，此时 destroy 会把回调正在使用的 socket/客户端对象拆掉。
 * http_finish 于是把收尾推迟到 zn_poller_async（下一个事件循环边界），
 * 届时回调栈已退栈，销毁安全。
 *
 * 回调前先把 status/location/body 拷到栈上：cb 一返回我们就 destroy，
 * 不能把指向自身字段的指针交给 cb 长期持有。 */
static void http_finish_async(void *user)
{
    zms_http_client *c = (zms_http_client *)user;
    zms_http_client_cb cb;
    void *cb_user;
    int status;
    char *body;
    size_t body_len;
    char location[512];

    if (!c) {
        return;
    }
    cb = c->cb;
    cb_user = c->user;
    status = c->status;
    body = c->body;
    body_len = c->body_len;
    snprintf(location, sizeof(location), "%s", c->location);
    c->cb = NULL;
    if (cb) {
        cb(status, location[0] ? location : NULL, body, body_len, cb_user);
    }
    zms_http_client_destroy(c);
}

/* 幂等的收尾入口：done 标志保证 cb 只会收到一次（超时与数据到达可能
 * 在同一 tick 竞争，先到的赢）。status>0 时覆盖状态；否则保留已解析到的
 * 状态码。收尾动作（取消定时器 + 异步回调销毁）委托给 http_finish_async，
 * poller 不可用时退化为同步执行（例如轮询线程已停）。 */
static void http_finish(zms_http_client *c, int status)
{
    if (!c || c->done) {
        return;
    }
    c->done = 1;
    if (status > 0) {
        c->status = status;
    } else if (c->status == 0) {
        c->status = 0;
    }
    if (c->timer) {
        zn_poller_timer_cancel(c->timer);
        c->timer = NULL;
    }
    if (c->poller && zn_poller_async(c->poller, http_finish_async, c, 0) == ZN_OK) {
        return;
    }
    http_finish_async(c);
}

static uint64_t http_on_timeout(void *user)
{
    zms_http_client *c = (zms_http_client *)user;
    if (!c) {
        return 0;
    }
    zn_warn("[webrtc-http] timeout");
    c->timer = NULL;
    /* 超时前若已拿到完整 SDP 仍接受。 */
    /* 某些服务端发完 body 却不及时关连接，死等 Content-Length 末尾之外
     * 会白等超时；此时只要状态 2xx 且有 body，就按成功返回。 */
    if (c->status >= 200 && c->status < 300 && c->body && c->body_len > 0) {
        http_finish(c, c->status);
        return 0;
    }
    http_finish(c, 0);
    return 0;
}

static void http_on_data(zms_http_client *c, const void *data, size_t len)
{
    if (!c || c->done) {
        return;
    }
    /* 硬上限：一个信令响应不该超过 256KB；超了基本是服务端异常（比如误把
     * 大文件当 SDP 回），直接判失败而不是无限吞数据把 rx 撑爆。 */
    if (c->rx_len + len > ZMS_HTTP_RX_MAX) {
        zn_warn("[webrtc-http] response too large");
        http_finish(c, 0);
        return;
    }
    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;
    c->rx[c->rx_len] = '\0';
    if (c->body_off == 0) {
        int r = http_parse_headers(c);
        if (r < 0) {
            http_finish(c, 0);
            return;
        }
        if (r == 0) {
            return;
        }
        /* 头一到就判状态：非 2xx 无需再等 body，立刻带着状态码收尾，
         * 让上层尽快看到 4xx/5xx（比如鉴权失败）。 */
        if (c->status < 200 || c->status >= 300) {
            http_finish(c, c->status);
            return;
        }
    }
    if (http_parse_body(c) == 1) {
        http_finish(c, c->status);
    }
}

static void http_tcp_on_recv(zn_tcp_client *client, const void *data, size_t len, void *user)
{
    (void)client;
    http_on_data((zms_http_client *)user, data, len);
}

static void http_tcp_on_connect(zn_tcp_client *client, void *user)
{
    zms_http_client *c = (zms_http_client *)user;
    (void)client;
    if (!c) {
        return;
    }
    zn_info("[webrtc-http] connected %s:%u", c->host, (unsigned)c->port);
    (void)zn_tcp_client_send(c->tcp, c->req, c->req_len);
}

static void http_tcp_on_error(zn_tcp_client *client, void *user)
{
    (void)client;
    zn_warn("[webrtc-http] tcp error");
    http_finish((zms_http_client *)user, 0);
}

static void http_tls_on_recv(zn_tls_client *client, const void *data, size_t len, void *user)
{
    (void)client;
    http_on_data((zms_http_client *)user, data, len);
}

static void http_tls_on_connect(zn_tls_client *client, void *user)
{
    zms_http_client *c = (zms_http_client *)user;
    (void)client;
    if (!c) {
        return;
    }
    zn_info("[webrtc-http] tls connected %s:%u", c->host, (unsigned)c->port);
    (void)zn_tls_client_send(c->tls, c->req, c->req_len);
}

static void http_tls_on_error(zn_tls_client *client, void *user)
{
    (void)client;
    zn_warn("[webrtc-http] tls error");
    http_finish((zms_http_client *)user, 0);
}

static int http_parse_url(zms_http_client *c, const char *url)
{
    const char *p;
    const char *slash;
    const char *colon;
    size_t host_len;

    if (!c || !url) {
        return -1;
    }
    if (strncmp(url, "https://", 8) == 0) {
        c->https = 1;
        c->port = 443;
        p = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        c->https = 0;
        c->port = 80;
        p = url + 7;
    } else {
        return -1;
    }
    slash = strchr(p, '/');
    if (!slash) {
        return -1;
    }
    colon = memchr(p, ':', (size_t)(slash - p));
    if (colon) {
        host_len = (size_t)(colon - p);
        c->port = (uint16_t)strtoul(colon + 1, NULL, 10);
    } else {
        host_len = (size_t)(slash - p);
    }
    if (host_len == 0 || host_len >= sizeof(c->host)) {
        return -1;
    }
    memcpy(c->host, p, host_len);
    c->host[host_len] = '\0';
    snprintf(c->path, sizeof(c->path), "%s", slash);
    return 0;
}

zms_http_client *zms_http_client_post(zn_poller *poller, const char *url, const char *content_type,
                                      const char *body, size_t body_len, const char *token,
                                      struct zn_ssl_ctx *ssl_ctx, int timeout_ms,
                                      zms_http_client_cb cb, void *user)
{
    zms_http_client *c;
    char host_hdr[192];
    size_t cap;
    int n;

    if (!poller || !url || !cb) {
        return NULL;
    }
    c = (zms_http_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->poller = poller;
    c->cb = cb;
    c->user = user;
    c->content_length = -1;
    if (http_parse_url(c, url) != 0) {
        zn_warn("[webrtc-http] bad url: %s", url);
        free(c);
        return NULL;
    }
    if (c->https && !ssl_ctx) {
        zn_warn("[webrtc-http] https without ssl ctx");
        free(c);
        return NULL;
    }
    /* 请求缓冲 = body + 定长头部（方法行/各头/结尾）+ token。1024 是给
     * 头部留的余量，远大于实际头长，避免为算精确长度引入分支。 */
    cap = body_len + 1024 + (token ? strlen(token) : 0);
    c->req = (char *)malloc(cap);
    c->rx = (char *)malloc(ZMS_HTTP_RX_MAX + 1);
    if (!c->req || !c->rx) {
        free(c->req);
        free(c->rx);
        free(c);
        return NULL;
    }
    /* Host 头只在非默认端口时带端口号——HTTP/1.1 默认端口省略是惯例，
     * 某些严格的反代/vhost 匹配会因多了默认端口而不认。 */
    if (c->port == (c->https ? 443u : 80u)) {
        snprintf(host_hdr, sizeof(host_hdr), "%s", c->host);
    } else {
        snprintf(host_hdr, sizeof(host_hdr), "%s:%u", c->host, (unsigned)c->port);
    }
    /* 一次性请求，Connection: close 最省事：服务端回完即关，client 无需
     * 维护 keep-alive 状态，也就无需 alpn/pipeline 处理。 */
    n = snprintf(c->req, cap,
                 "POST %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %zu\r\n"
                 "Accept: application/sdp\r\n"
                 "%s%s%s"
                 "Connection: close\r\n"
                 "\r\n",
                 c->path, host_hdr, content_type ? content_type : "application/sdp", body_len,
                 token ? "Authorization: Bearer " : "", token ? token : "", token ? "\r\n" : "");
    if (n < 0 || (size_t)n + body_len >= cap) {
        free(c->req);
        free(c->rx);
        free(c);
        return NULL;
    }
    if (body_len > 0) {
        memcpy(c->req + n, body, body_len);
        n += (int)body_len;
    }
    c->req_len = (size_t)n;
    c->rx[0] = '\0';

    {
    char ipbuf[64];

    if (c->https) {
        zn_tls_client_ops_t ops;
        zn_tls_client_opts_t opts;

        memset(&ops, 0, sizeof(ops));
        ops.on_connect = http_tls_on_connect;
        ops.on_recv = http_tls_on_recv;
        ops.on_error = http_tls_on_error;
        memset(&opts, 0, sizeof(opts));
        opts.poller = poller;
        opts.ssl_ctx = ssl_ctx;
        opts.ops = &ops;
        opts.user = c;
        opts.sni_host = c->host;
        c->tls = zn_tls_client_create(&opts);
        if (!c->tls || zn_tls_client_connect(c->tls, zms_dns_target(c->host, ipbuf, sizeof(ipbuf)),
                                             c->port) != ZN_OK) {
            zn_warn("[webrtc-http] tls connect failed %s:%u", c->host, (unsigned)c->port);
            goto fail;
        }
    } else {
        zn_tcp_client_ops_t ops;
        zn_tcp_client_opts_t opts;

        memset(&ops, 0, sizeof(ops));
        ops.on_connect = http_tcp_on_connect;
        ops.on_recv = http_tcp_on_recv;
        ops.on_error = http_tcp_on_error;
        memset(&opts, 0, sizeof(opts));
        opts.poller = poller;
        opts.ops = &ops;
        opts.user = c;
        c->tcp = zn_tcp_client_create(&opts);
        if (!c->tcp || zn_tcp_client_connect(c->tcp, zms_dns_target(c->host, ipbuf, sizeof(ipbuf)),
                                             c->port) != ZN_OK) {
            zn_warn("[webrtc-http] tcp connect failed %s:%u", c->host, (unsigned)c->port);
            goto fail;
        }
    }
    }
    c->timer = zn_poller_do_delay(poller, timeout_ms > 0 ? (uint64_t)timeout_ms : 10000,
                                   http_on_timeout, c);
    zn_info("[webrtc-http] POST %s:%u%s body=%zu", c->host, (unsigned)c->port, c->path,
             body_len);
    return c;

fail:
    if (c->tcp) {
        zn_tcp_client_destroy(c->tcp);
    }
    if (c->tls) {
        zn_tls_client_destroy(c->tls);
    }
    free(c->req);
    free(c->rx);
    if (c->body_alloc) {
        free(c->body);
    }
    free(c);
    return NULL;
}

void zms_http_client_destroy(zms_http_client *c)
{
    if (!c) {
        return;
    }
    if (c->timer) {
        zn_poller_timer_cancel(c->timer);
        c->timer = NULL;
    }
    if (c->tcp) {
        zn_tcp_client_close(c->tcp);
        zn_tcp_client_destroy(c->tcp);
        c->tcp = NULL;
    }
    if (c->tls) {
        zn_tls_client_close(c->tls);
        zn_tls_client_destroy(c->tls);
        c->tls = NULL;
    }
    free(c->req);
    free(c->rx);
    if (c->body_alloc) {
        free(c->body);
    }
    free(c);
}
