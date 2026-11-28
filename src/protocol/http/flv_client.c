/**
 * @file flv_client.c
 * @brief HTTP-FLV 拉流客户端。
 */
#include "zcs/protocol/http/flv_client.h"
#include "zcs/util/dns_cache.h"
#include "zcs/ops/config/zcs_have_tls.h"
#include "zcs/media/container/flv/flv_tag_demuxer.h"
#include "zcs/media/container/flv/flv_tag_framer.h"
#include "zcs/media/wire/rtmp.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/protocol/http/parser.h"
#include "zcs/protocol/http/message_framer.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tls_client.h"
#include "zn/util/log.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLV_IO_BUF (256 * 1024)
typedef struct zcs_http_flv_url {
    char host[256];
    uint16_t port;
    char path[ZCS_HTTP_URL_MAX];
    int use_tls;
} zcs_http_flv_url;

typedef enum {
    BODY_CL = 0,
    BODY_CHUNKED,
    BODY_UNTIL_CLOSE,
} http_body_mode;

typedef struct zcs_http_flv_chunk_dec {
    int in_chunk;
    size_t chunk_left;
    char line[32];
    size_t line_len;
} zcs_http_flv_chunk_dec;

struct zcs_http_flv_client {
    zcs_http_flv_client_opts opts;
    zcs_http_flv_url url;
    zn_tcp_client *tcp;
    zn_tls_client *tls;
    zcs_http_message_framer *http;
    zcs_flv_tag_demuxer *demux;
    zcs_http_parser http_hdr;
    http_body_mode body_mode;
    zcs_http_flv_chunk_dec chunk;
    zcs_flv_tag_framer *flv_bs;
    int stopping;
    int ready_sent;
    char req_buf[4096];
};

/* 单条错误上报：stopping 后不再上报——停止流程自己会 close，非预期的错误
 * 回调（如 close 触发的读错误）不应再扰动调用方。 */
static void client_fail(zcs_http_flv_client *c, zn_err_t err)
{
    if (!c || c->stopping) {
        return;
    }
    if (c->opts.on_error) {
        c->opts.on_error(err, c->opts.user);
    }
}

static void client_send(zcs_http_flv_client *c, const void *data, size_t len)
{
    if (!c || !data || !len) {
        return;
    }
#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        zn_tls_client_send(c->tls, data, len);
        return;
    }
#endif
    if (c->tcp) {
        zn_tcp_client_send(c->tcp, data, len);
    }
}

static zn_err_t client_connect(zcs_http_flv_client *c)
{
    if (!c) {
        return ZN_ERR_INVALID;
    }
    {
        char ipbuf[64];
        const char *target = zcs_dns_target(c->url.host, ipbuf, sizeof(ipbuf));

#if ZCS_HAVE_PULL_TLS
        if (c->tls) {
            return zn_tls_client_connect(c->tls, target, c->url.port);
        }
#endif
        if (c->tcp) {
            return zn_tcp_client_connect(c->tcp, target, c->url.port);
        }
    }
    return ZN_ERR_STATE;
}

static void client_close(zcs_http_flv_client *c)
{
    if (!c) {
        return;
    }
#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        zn_tls_client_close(c->tls);
        return;
    }
#endif
    if (c->tcp) {
        zn_tcp_client_close(c->tcp);
    }
}

/*
 * 解析 http(s)://host[:port]/path 形式的 URL。手写解析而非依赖通用 URL 库：
 * 这里只需要 host/port/path 三项，且要容忍常见写法差异。
 *   - 默认端口随 scheme（http=80 / https=443）；
 *   - host 与 path 的分界是第一个 '/'；端口以 host 段内第一个 ':' 分隔；
 *   - 显式写了 ":0" 视为缺省端口；
 *   - 无 path 时补 "/"，保证请求行合法。
 */
static zn_err_t parse_http_flv_url(const char *url, zcs_http_flv_url *out)
{
    if (!url || !out) {
        return ZN_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        out->use_tls = 1;
        out->port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        out->port = 80;
        p += 7;
    } else {
        return ZN_ERR_INVALID;
    }

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = NULL;
    for (const char *c = p; c < host_end; ++c) {
        if (*c == ':') {
            colon = c;
            break;
        }
    }
    size_t host_len = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return ZN_ERR_INVALID;
    }
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';
    if (colon) {
        out->port = (uint16_t)atoi(colon + 1);
        if (out->port == 0) {
            out->port = out->use_tls ? 443 : 80;
        }
    }

    if (slash) {
        strncpy(out->path, slash, sizeof(out->path) - 1);
    } else {
        strncpy(out->path, "/", sizeof(out->path) - 1);
    }
    return ZN_OK;
}

/* Host 头只在非默认端口时带 ":port"：标准端口省略是 HTTP/1.1 惯例，且某些
 * 虚拟主机/CDN 对带默认端口的 Host 匹配更严格，省掉更稳。 */
static void send_http_get(zcs_http_flv_client *c)
{
    char host_line[320];
    if ((!c->url.use_tls && c->url.port != 80) || (c->url.use_tls && c->url.port != 443)) {
        snprintf(host_line, sizeof(host_line), "%s:%u", c->url.host, (unsigned)c->url.port);
    } else {
        snprintf(host_line, sizeof(host_line), "%s", c->url.host);
    }

    int n = snprintf(c->req_buf, sizeof(c->req_buf),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Connection: keep-alive\r\n"
                     "User-Agent: ZCS-HttpFlv/1.0\r\n"
                     "Accept: */*\r\n"
                     "\r\n",
                     c->url.path[0] ? c->url.path : "/", host_line);
    if (n > 0 && (size_t)n < sizeof(c->req_buf)) {
        client_send(c, c->req_buf, (size_t)n);
    }
}

/*
 * 判断一个 FLV tag 是否是“配置帧”（音视频 sequence header / end-of-seq）。
 *
 * 用途见 maybe_ready：只有当收到**非配置**的音视频帧时才认为流真正可播。
 * 因为客户端通常先发 sequence header（含 SPS/PPS/ASC），此时下游还没法解帧；
 * 提前报 ready 会让调用方以为已有画面而立刻起播，拿到空帧。
 * len<2 时返回 1：太短无法识别，保守地当作配置帧跳过，不触发 ready。
 */
static int is_config_flv_body(uint8_t type, const uint8_t *body, size_t len)
{
    if (!body || len < 2) {
        return 1;
    }
    if (type == ZCS_RTMP_MSG_VIDEO) {
        zcs_codec_id vc = zcs_flv_tag_video_codec(body, len);
        if (vc == ZCS_CODEC_H264 || vc == ZCS_CODEC_H265) {
            zcs_flv_tag_packet_kind pt = zcs_flv_tag_video_packet_kind(body, len);
            return pt == ZCS_FLV_TAG_PKT_SEQ_HEADER || pt == ZCS_FLV_TAG_PKT_END_OF_SEQ;
        }
    }
    if (type == ZCS_RTMP_MSG_AUDIO) {
        if (zcs_flv_tag_audio_codec(body, len) == ZCS_CODEC_AAC) {
            return zcs_flv_tag_audio_packet_kind(body, len) == ZCS_FLV_TAG_PKT_SEQ_HEADER;
        }
    }
    return 0;
}

static void maybe_ready(zcs_http_flv_client *c, uint8_t type, const uint8_t *body, size_t len)
{
    if (c->ready_sent || !c->opts.on_ready) {
        return;
    }
    if (type != ZCS_RTMP_MSG_AUDIO && type != ZCS_RTMP_MSG_VIDEO) {
        return;
    }
    if (is_config_flv_body(type, body, len)) {
        return;
    }
    c->ready_sent = 1;
    c->opts.on_ready(c->opts.user);
}

static void on_demux_frame(const zcs_frame *frame, void *user)
{
    zcs_http_flv_client *c = (zcs_http_flv_client *)user;
    if (!c || c->stopping || !frame) {
        return;
    }
    if (c->opts.on_frame) {
        c->opts.on_frame(frame, c->opts.user);
    }
}

/* 把 FLV tag 派发给上层。两条出口互斥：on_media 优先（原始 tag，调用方自行
 * 解封），否则交给内部 demuxer 拆成帧走 on_frame。return 保证二选一不重复。 */
static void feed_tag(zcs_http_flv_client *c, uint8_t type, const uint8_t *body, size_t len,
                     uint32_t ts)
{
    uint8_t msg;

    if (!c || !body || !len) {
        return;
    }
    maybe_ready(c, type, body, len);
    msg = (type == ZCS_RTMP_MSG_VIDEO)   ? ZCS_RTMP_MSG_VIDEO
          : (type == ZCS_RTMP_MSG_AUDIO) ? ZCS_RTMP_MSG_AUDIO
                                         : ZCS_RTMP_MSG_DATA;
    if (c->opts.on_media) {
        c->opts.on_media(msg, ts, body, len, c->opts.user);
        return;
    }
    if (c->demux) {
        (void)zcs_flv_tag_demuxer_input(c->demux, msg, body, len, ts);
    }
}

static void flv_reset(zcs_http_flv_client *c)
{
    if (c && c->flv_bs) {
        zcs_flv_tag_framer_reset(c->flv_bs);
    }
}

static void on_flv_byte_tag(uint8_t type, const uint8_t *body, size_t len, uint32_t ts_ms,
                            void *user)
{
    zcs_http_flv_client *c = (zcs_http_flv_client *)user;
    if (!c || c->stopping) {
        return;
    }
    feed_tag(c, type, body, len, ts_ms);
}

static void flv_feed(zcs_http_flv_client *c, const uint8_t *data, size_t len)
{
    zn_err_t err;
    if (!c || !c->flv_bs || !data || len == 0) {
        return;
    }
    err = zcs_flv_tag_framer_feed(c->flv_bs, data, len, on_flv_byte_tag, c);
    if (err == ZN_ERR_INVALID) {
        client_fail(c, ZN_ERR_INVALID);
    } else if (err == ZN_ERR_NOMEM || err == ZN_ERR_BUFFER_TOO_SMALL) {
        client_fail(c, ZN_ERR_NOMEM);
    }
}

/* 从已累积的一行里解出十六进制块长（chunk size）。遇到 \r 或 \n 即视为行结束，
 * 用 strtoul 按十六进制解析；解析失败得到 0，会被上层当作“末块”结束 body。 */
static int chunk_line_complete(zcs_http_flv_chunk_dec *d, size_t *out_size)
{
    for (size_t i = 0; i < d->line_len; ++i) {
        if (d->line[i] == '\r' || d->line[i] == '\n') {
            d->line[i] = '\0';
            unsigned long sz = strtoul(d->line, NULL, 16);
            *out_size = (size_t)sz;
            return 1;
        }
    }
    return 0;
}

/*
 * HTTP chunked 传输的解码器。
 *
 * 状态机：先读一行“十六进制块长\r\n”，再读该长度的数据块，块后跟 \r\n；
 * 块长为 0 表示 body 结束。line 缓冲有上限（32 字节），超长视为非法——
 * 合法块长行不会那么长，出现超长说明数据流已错位，及时失败而不是无限吞。
 */
static void chunk_feed(zcs_http_flv_client *c, const uint8_t *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (!c->chunk.in_chunk) {
            while (off < len) {
                if (c->chunk.line_len + 1 >= sizeof(c->chunk.line)) {
                    client_fail(c, ZN_ERR_INVALID);
                    return;
                }
                c->chunk.line[c->chunk.line_len++] = (char)data[off++];
                size_t sz = 0;
                if (!chunk_line_complete(&c->chunk, &sz)) {
                    continue;
                }
                c->chunk.line_len = 0;
                if (sz == 0) {
                    c->chunk.in_chunk = 0;
                    return;
                }
                c->chunk.chunk_left = sz;
                c->chunk.in_chunk = 1;
                break;
            }
            continue;
        }
        size_t take = c->chunk.chunk_left;
        if (take > len - off) {
            take = len - off;
        }
        flv_feed(c, data + off, take);
        off += take;
        c->chunk.chunk_left -= take;
        if (c->chunk.chunk_left == 0) {
            c->chunk.in_chunk = 0;
            while (off < len && (data[off] == '\r' || data[off] == '\n')) {
                ++off;
            }
        }
    }
}

/* body 按传输模式分流：chunked 先解块再喂 FLV，其余（定长/直至关闭）直接喂。
 * BODY_UNTIL_CLOSE 依赖连接关闭判定 body 结束，在直播场景里就是常态。 */
static void body_feed(zcs_http_flv_client *c, const void *data, size_t len)
{
    if (!c || !data || len == 0) {
        return;
    }
    const uint8_t *d = (const uint8_t *)data;
    if (c->body_mode == BODY_CHUNKED) {
        chunk_feed(c, d, len);
    } else {
        flv_feed(c, d, len);
    }
}

static int str_icontains(const char *hay, const char *needle)
{
    if (!hay || !needle) {
        return 0;
    }
    size_t nlen = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == nlen) {
            return 1;
        }
    }
    return 0;
}

/*
 * HTTP 响应头回调。
 *
 * 返回值即 framer 期望的 body 长度语义：
 *   >0 = Content-Length 指定的按字节数读取（读完即止）；
 *   -1 = 长度未知，按 chunked 或直到连接关闭处理。
 * 这样 framer 不必理解传输细节，只需按返回值切分 body 回调。
 *
 * 同时接受 200 与 206：直播通常 200，但部分服务端对 Range 请求回 206，
 * 二者 body 都是 FLV，放行以免误判失败。
 */
static intptr_t on_http_header(const char *header, size_t header_len, void *user)
{
    zcs_http_flv_client *c = (zcs_http_flv_client *)user;
    if (!c) {
        return 0;
    }

    zcs_http_parser_clear(&c->http_hdr);
    if (zcs_http_parser_parse_header(&c->http_hdr, header, header_len) != ZN_OK) {
        return 0;
    }

    if (!c->http_hdr.is_response) {
        zn_warn("http-flv not an HTTP response (first=%s)", c->http_hdr.method);
        client_fail(c, ZN_ERR_INVALID);
        return 0;
    }
    int status = atoi(c->http_hdr.url);
    if (status != 200 && status != 206) {
        zn_warn("http-flv bad status %d", status);
        client_fail(c, ZN_ERR_IO);
        return 0;
    }

    const char *ct = zcs_http_parser_header(&c->http_hdr, "Content-Type");
    if (ct[0] && !str_icontains(ct, "flv") && !str_icontains(ct, "octet-stream")) {
        zn_warn("http-flv unexpected content-type: %s", ct);
    }

    const char *te = zcs_http_parser_header(&c->http_hdr, "Transfer-Encoding");
    const char *cl = zcs_http_parser_header(&c->http_hdr, "Content-Length");
    memset(&c->chunk, 0, sizeof(c->chunk));
    flv_reset(c);

    if (te[0] && str_icontains(te, "chunked")) {
        c->body_mode = BODY_CHUNKED;
        return -1;
    }
    if (cl[0]) {
        unsigned long long n = 0;
        for (const char *p = cl; *p; ++p) {
            if (*p < '0' || *p > '9') {
                break;
            }
            n = n * 10ULL + (unsigned long long)(*p - '0');
        }
        if (n > 0) {
            c->body_mode = BODY_CL;
            return (intptr_t)n;
        }
    }
    c->body_mode = BODY_UNTIL_CLOSE;
    return -1;
}

static void on_http_content(const char *data, size_t len, void *user)
{
    zcs_http_flv_client *c = (zcs_http_flv_client *)user;
    if (!c || c->stopping) {
        return;
    }
    body_feed(c, data, len);
}

static void on_transport_data(zcs_http_flv_client *c, const void *data, size_t len)
{
    if (!c->http) {
        return;
    }
    zn_err_t err = zcs_http_message_framer_input(c->http, data, len);
    if (err != ZN_OK) {
        client_fail(c, err);
    }
}

/* 重连/重播时清空所有解析状态：HTTP 分帧、头部解析器、chunk 解码器、FLV 组帧器
 * 都要从干净状态开始，否则上一次残留的半帧会被拼到新连接的流前面导致错位。 */
static void on_io_connected(zcs_http_flv_client *c)
{
    zcs_http_message_framer_reset(c->http);
    zcs_http_parser_clear(&c->http_hdr);
    memset(&c->chunk, 0, sizeof(c->chunk));
    flv_reset(c);
    c->ready_sent = 0;
    send_http_get(c);
}

static void on_tcp_connect(zn_tcp_client *tcp, void *user)
{
    (void)tcp;
    on_io_connected((zcs_http_flv_client *)user);
}

static void on_tcp_recv(zn_tcp_client *tcp, const void *data, size_t len, void *user)
{
    (void)tcp;
    on_transport_data((zcs_http_flv_client *)user, data, len);
}

static void on_tcp_error(zn_tcp_client *tcp, void *user)
{
    (void)tcp;
    client_fail((zcs_http_flv_client *)user, ZN_ERR_IO);
}

static void on_tls_connect(zn_tls_client *tls, void *user)
{
    (void)tls;
    on_io_connected((zcs_http_flv_client *)user);
}

static void on_tls_recv(zn_tls_client *tls, const void *data, size_t len, void *user)
{
    (void)tls;
    on_transport_data((zcs_http_flv_client *)user, data, len);
}

static void on_tls_error(zn_tls_client *tls, void *user)
{
    (void)tls;
    client_fail((zcs_http_flv_client *)user, ZN_ERR_IO);
}

zcs_http_flv_client *zcs_http_flv_client_create(const zcs_http_flv_client_opts *opts)
{
    if (!opts || !opts->poller || !opts->url || (!opts->on_media && !opts->on_frame)) {
        return NULL;
    }

    zcs_http_flv_client *c = (zcs_http_flv_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opts = *opts;
    if (parse_http_flv_url(opts->url, &c->url) != ZN_OK) {
        free(c);
        return NULL;
    }

    if (opts->on_frame) {
        zcs_flv_tag_demuxer_opts dopts = {on_demux_frame, c};

        c->demux = zcs_flv_tag_demuxer_create(&dopts);
        if (!c->demux) {
            free(c);
            return NULL;
        }
    }
    c->flv_bs = zcs_flv_tag_framer_create();
    if (!c->flv_bs) {
        zcs_flv_tag_demuxer_destroy(c->demux);
        free(c);
        return NULL;
    }

    zcs_http_message_framer_opts sopts = {on_http_header, on_http_content, c};
    c->http = zcs_http_message_framer_create(&sopts);
    if (!c->http) {
        zcs_flv_tag_demuxer_destroy(c->demux);
        free(c);
        return NULL;
    }

/* https 拉流依赖 OpenSSL 与调用方提供的 ssl_ctx：缺任一都直接失败——没有 TLS
 * 能力还硬连只会得到难以诊断的 I/O 错误，不如在创建阶段明确拒绝。 */
    if (c->url.use_tls) {
#if !defined(ZN_HAVE_OPENSSL) || !ZN_HAVE_OPENSSL
        zcs_http_flv_client_destroy(c);
        return NULL;
#else
        if (!opts->ssl_ctx) {
            zcs_http_flv_client_destroy(c);
            return NULL;
        }
        zn_tls_client_ops_t tops = {on_tls_connect, on_tls_recv, on_tls_error};
        zn_tls_client_opts_t topts = {opts->poller, opts->ssl_ctx, &tops, c, c->url.host};
        c->tls = zn_tls_client_create(&topts);
        if (!c->tls) {
            zcs_http_flv_client_destroy(c);
            return NULL;
        }
#endif
    } else {
        zn_tcp_client_ops_t tops = {on_tcp_connect, on_tcp_recv, on_tcp_error};
        zn_tcp_client_opts_t topts = {opts->poller, &tops, c};
        c->tcp = zn_tcp_client_create(&topts);
        if (!c->tcp) {
            zcs_http_flv_client_destroy(c);
            return NULL;
        }
    }
    return c;
}

void zcs_http_flv_client_destroy(zcs_http_flv_client *c)
{
    if (!c) {
        return;
    }
    zcs_http_flv_client_stop(c);
    zn_tcp_client_destroy(c->tcp);
#if ZCS_HAVE_PULL_TLS
    zn_tls_client_destroy(c->tls);
#endif
    zcs_http_message_framer_destroy(c->http);
    zcs_flv_tag_demuxer_destroy(c->demux);
    zcs_flv_tag_framer_destroy(c->flv_bs);
    zcs_http_parser_clear(&c->http_hdr);
    free(c);
}

zn_err_t zcs_http_flv_client_play(zcs_http_flv_client *c)
{
    if (!c) {
        return ZN_ERR_INVALID;
    }
    c->stopping = 0;
    c->ready_sent = 0;
    return client_connect(c);
}

void zcs_http_flv_client_stop(zcs_http_flv_client *c)
{
    if (!c) {
        return;
    }
    c->stopping = 1;
    client_close(c);
    if (c->http) {
        zcs_http_message_framer_reset(c->http);
    }
    flv_reset(c);
    c->ready_sent = 0;
}
