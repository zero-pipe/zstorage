/**
 * @file hls_client.c
 * @brief HTTP-HLS 拉流客户端（拉取 m3u8 与分片）。
 *
 * 状态机（简化）：
 *   play → 拉 m3u8 →(master 则再拉一次 media playlist)→ 顺序拉分片 → 分片播完
 *   → live 则按 target_duration/2 定时重拉 m3u8 → 循环；VOD(ENDLIST) 则到末尾停止。
 *   任何一步失败（网络/HTTP 非 200/半包）都走软重试（hls_soft_retry）而不是直接
 *   报错终止——拉流代理面对的上游常是“刚起来还没切片”的状态，硬失败会让观众
 *   永远起不来。
 *
 * 并发假设：整套回调（TCP/TLS/poller timer）都跑在该客户端所属 poller 的单线程里，
 * 结构体字段无需加锁。stopping 标志是“已请求停止但回调可能还在途”的闸门，每个
 * 回调入口先检查它，防止 stop 之后仍产生副作用。
 */
#include "zcs/protocol/http/hls_client.h"
#include "zcs/util/dns_cache.h"
#include "zcs/ops/config/zcs_have_tls.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/clock.h"
#include "mpeg-ts.h"
#include "mpeg-types.h"
#include "zcs/protocol/http/parser.h"
#include "zcs/protocol/http/message_framer.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tls_client.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 分片数上限 128：单次播放列表只保留最近约 128 个分片，超出部分是老分片，
 * 直播场景下没人会回看那么远；也防止恶意/异常超长列表撑爆内存。
 * 单请求 body 上限 32MB：远大于正常分片，只是兜住异常大响应避免无界增长。 */
#define HLS_SEG_MAX 128
#define HLS_BODY_MAX (32U * 1024U * 1024U)
#define HLS_URL_MAX 2048

typedef struct zcs_http_hls_url {
    char host[256];
    uint16_t port;
    char path[ZCS_HTTP_URL_MAX];
    int use_tls;
} zcs_http_hls_url;

/**
 * 播放列表里的一个分片。
 * uri 在堆上（按实际长度分配）：128 × 2048 的定长数组会让每路 HLS 拉流
 * 白占 256KB，而真实 URI 只有几十字节。
 */
typedef struct zcs_http_hls_segment {
    char *uri;
    double duration;
} zcs_http_hls_segment;

typedef enum {
    HLS_REQ_M3U8 = 0,
    HLS_REQ_TS,
} hls_req_kind;

typedef enum {
    BODY_CL = 0,
    BODY_CHUNKED,
    BODY_UNTIL_CLOSE,
} http_body_mode;

typedef struct zcs_http_hls_chunk_dec {
    int in_chunk;
    size_t chunk_left;
    char line[32];
    size_t line_len;
} zcs_http_hls_chunk_dec;

struct zcs_http_hls_client {
    zcs_http_hls_client_opts opts;
    zcs_http_hls_url base_url;
    char playlist_url[HLS_URL_MAX];
    char fetch_url[HLS_URL_MAX];
    hls_req_kind req_kind;
    zn_tcp_client *tcp;
    zn_tls_client *tls;
    zcs_http_message_framer *http;
    zcs_http_parser http_hdr;
    http_body_mode body_mode;
    zcs_http_hls_chunk_dec chunk;
    uint8_t *body;
    size_t body_len;
    size_t body_cap;
    zcs_http_hls_segment segs[HLS_SEG_MAX];
    size_t seg_count;
    size_t seg_pos;
    int live;
    int is_master_pending;
    double target_duration;
    char last_seg_uri[HLS_URL_MAX];
    struct ts_demuxer_t *ts_demuxer;
    zn_poller_timer *refresh_timer;
    size_t expect_body;
    int stopping;
    int ready_sent;
    int http_active;
    int pending_soft_retry;
    int connect_retries;
    char req_buf[4096];
};

static void on_http_done(zcs_http_hls_client *c, zn_err_t err);
static void start_next_segment(zcs_http_hls_client *c);
static uint64_t refresh_task(void *user);
static void hls_soft_retry(zcs_http_hls_client *c, uint64_t delay_ms);
static void client_close_http(zcs_http_hls_client *c);

static zcs_codec_id codecid_to_ZCS(int codecid)
{
    return zcs_codec_from_mpeg_psi(codecid);
}

static void client_fail(zcs_http_hls_client *c, zn_err_t err)
{
    if (!c || c->stopping) {
        return;
    }
    client_close_http(c);
    zn_warn("hls_pull fatal err=%d kind=%d url=%s body=%u", (int)err, (int)c->req_kind,
             c->fetch_url, (unsigned)c->body_len);
    if (c->opts.on_error) {
        c->opts.on_error(err, c->opts.user);
    }
}

static void cancel_refresh(zcs_http_hls_client *c)
{
    if (c && c->refresh_timer) {
        zn_poller_timer_cancel(c->refresh_timer);
        c->refresh_timer = NULL;
    }
}

static void client_close_http(zcs_http_hls_client *c)
{
    if (!c) {
        return;
    }
    c->http_active = 0;
#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        zn_tls_client_close(c->tls);
        return;
    }
#endif
    if (c->tcp) {
        zn_tcp_client_close(c->tcp);
    }
    if (c->http) {
        zcs_http_message_framer_reset(c->http);
    }
}

static void body_reset(zcs_http_hls_client *c)
{
    c->body_len = 0;
    memset(&c->chunk, 0, sizeof(c->chunk));
}

/* 追加 body 数据。容量翻倍扩容（起点 64KB）：分片通常几百 KB~数 MB，翻倍
 * 摊销拷贝成本。tosmall 返回而非静默截断，让上层把“超限”当 IO 错误处理。 */
static zn_err_t body_append(zcs_http_hls_client *c, const void *data, size_t len)
{
    if (!c || !data || len == 0) {
        return ZN_OK;
    }
    if (c->body_len + len > HLS_BODY_MAX) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    if (c->body_len + len > c->body_cap) {
        size_t cap = c->body_cap ? c->body_cap : 65536;
        while (cap < c->body_len + len) {
            cap *= 2;
        }
        uint8_t *p = (uint8_t *)realloc(c->body, cap);
        if (!p) {
            return ZN_ERR_NOMEM;
        }
        c->body = p;
        c->body_cap = cap;
    }
    memcpy(c->body + c->body_len, data, len);
    c->body_len += len;
    return ZN_OK;
}

static zn_err_t parse_http_url(const char *url, zcs_http_hls_url *out)
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

static size_t hls_url_prefix_len(const char *scheme, const char *host, uint16_t port)
{
    char port_buf[8];
    int pn = snprintf(port_buf, sizeof(port_buf), "%u", (unsigned)port);
    size_t port_len = pn > 0 ? (size_t)pn : 1;

    return strlen(scheme) + 3 + strlen(host) + 1 + port_len;
}

/* 相对分片 URL 解析：绝对 URL 直接透传；以 '/' 开头的是站点绝对路径（复用 base 的
 * scheme/host/port）；其余相对当前 playlist 所在目录拼接。裁剪 path 时用 %.*s 按
 * 剩余容量截断，避免 snprintf 溢出告警，同时保证不越界。 */
static void resolve_hls_url(const char *base, const char *rel, char *out, size_t cap)
{
    if (!rel || !rel[0] || !out || cap == 0) {
        return;
    }
    if (strncmp(rel, "http://", 7) == 0 || strncmp(rel, "https://", 8) == 0) {
        strncpy(out, rel, cap - 1);
        out[cap - 1] = '\0';
        return;
    }
    char scheme[16] = "http";
    char host[256] = "";
    uint16_t port = 80;
    const char *p = base;
    if (strncmp(p, "https://", 8) == 0) {
        strcpy(scheme, "https");
        port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = strchr(p, ':');
    if (colon && colon < host_end) {
        size_t hl = (size_t)(colon - p);
        if (hl < sizeof(host)) {
            memcpy(host, p, hl);
            host[hl] = '\0';
        }
        port = (uint16_t)atoi(colon + 1);
    } else {
        size_t hl = (size_t)(host_end - p);
        if (hl < sizeof(host)) {
            memcpy(host, p, hl);
            host[hl] = '\0';
        }
    }
    char dir[ZCS_HTTP_URL_MAX];
    if (slash) {
        strncpy(dir, slash, sizeof(dir) - 1);
    } else {
        strcpy(dir, "/");
    }
    char *last = strrchr(dir, '/');
    if (last) {
        last[1] = '\0';
    } else {
        strcat(dir, "/");
    }

    {
        size_t prefix = hls_url_prefix_len(scheme, host, port);
        size_t path_max = cap > prefix ? cap - prefix : 0;

        if (rel[0] == '/') {
            size_t rel_len = strlen(rel);
            if (rel_len > path_max) {
                rel_len = path_max;
            }
            snprintf(out, cap, "%s://%s:%u%.*s", scheme, host, (unsigned)port, (int)rel_len, rel);
        } else {
            char path[ZCS_HTTP_URL_MAX];
            size_t path_len;

            snprintf(path, sizeof(path), "%s%s", dir, rel);
            path_len = strlen(path);
            if (path_len > path_max) {
                path_len = path_max;
            }
            snprintf(out, cap, "%s://%s:%u%.*s", scheme, host, (unsigned)port, (int)path_len, path);
        }
    }
}

static char *trim_line(char *s)
{
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) {
        ++s;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

/* 判断一个 URI 是否当作 TS 分片。显式 .ts 直接接受；显式 .m3u8/.m4s/.mp4 否；
 * 其余“无扩展名或未知扩展名”一律按 TS 处理——很多打包器分片路径不带 .ts，
 * 宽松策略比严格白名单更能兼容各种上游。 */
static int uri_is_ts(const char *uri)
{
    if (!uri) {
        return 0;
    }
    size_t n = strlen(uri);
#ifdef _WIN32
    if (n >= 3 && _stricmp(uri + n - 3, ".ts") == 0)
#else
    if (n >= 3 && strcasecmp(uri + n - 3, ".ts") == 0)
#endif
        return 1;
    if (strstr(uri, ".m3u8") || strstr(uri, ".m4s") || strstr(uri, ".mp4")) {
        return 0;
    }
    return 1;
}

static void hls_segs_clear(zcs_http_hls_client *c)
{
    size_t i;

    if (!c) {
        return;
    }
    for (i = 0; i < c->seg_count && i < HLS_SEG_MAX; ++i) {
        free(c->segs[i].uri);
        c->segs[i].uri = NULL;
    }
    c->seg_count = 0;
}

static char *hls_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);

    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

/*
 * 解析 m3u8。两类清单：
 *   - master（含 #EXT-X-STREAM-INF）：只有子清单 URL，不切片。取第一个子清单的
 *     URL 写进 fetch_url 并置 is_master_pending，外层会再拉一次 media playlist。
 *   - media：由 #EXTINF:<dur> 下一行给出分片 URI 组成，按 #EXTINF 与 URI 成对消费。
 * 默认 live=1，只有出现 #EXT-X-ENDLIST 才降为 VOD；解析不到 #EXT-X-TARGETDURATION
 * 时用 2s 兜底（影响刷新间隔）。
 * 一旦看到 STREAM-INF 后的第一个非注释行就提前返回（只跟随一路码率，选码率不是本
 * 客户端职责）。
 */
static zn_err_t parse_m3u8(zcs_http_hls_client *c, const char *text, size_t len)
{
    if (!c || !text) {
        return ZN_ERR_INVALID;
    }
    hls_segs_clear(c);
    c->live = 1;
    c->target_duration = 2.0;
    double pending_dur = 0;
    int wait_uri = 0;
    int saw_stream_inf = 0;

    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        return ZN_ERR_NOMEM;
    }
    memcpy(buf, text, len);
    buf[len] = '\0';

    for (char *line = buf, *next = NULL; line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        line = trim_line(line);
        if (!line[0]) {
            continue;
        }
        if (line[0] != '#') {
            if (saw_stream_inf) {
                resolve_hls_url(c->playlist_url, line, c->fetch_url, sizeof(c->fetch_url));
                saw_stream_inf = 0;
                c->is_master_pending = 1;
                free(buf);
                return ZN_OK;
            }
            if (wait_uri && c->seg_count < HLS_SEG_MAX && uri_is_ts(line)) {
                char resolved[HLS_URL_MAX];
                zcs_http_hls_segment *seg;

                resolve_hls_url(c->playlist_url, line, resolved, sizeof(resolved));
                seg = &c->segs[c->seg_count];
                seg->uri = hls_strdup(resolved);
                if (!seg->uri) {
                    free(buf);
                    return ZN_ERR_NOMEM;
                }
                seg->duration = pending_dur > 0 ? pending_dur : c->target_duration;
                ++c->seg_count;
                wait_uri = 0;
                pending_dur = 0;
            }
            continue;
        }
        if (strncmp(line, "#EXT-X-ENDLIST", 14) == 0) {
            c->live = 0;
        } else if (strncmp(line, "#EXT-X-TARGETDURATION:", 22) == 0) {
            c->target_duration = atof(line + 22);
        } else if (strncmp(line, "#EXTINF:", 8) == 0) {
            pending_dur = atof(line + 8);
            wait_uri = 1;
        } else if (strncmp(line, "#EXT-X-STREAM-INF:", 18) == 0) {
            saw_stream_inf = 1;
        }
    }
    free(buf);
    return ZN_OK;
}

/*
 * TS demuxer 回调：把一个已解析的 ES 帧交给上层。
 * ready_sent 保证 on_ready 只触发一次——上层据此开始计时/建索引，重复触发会重置状态。
 * codec 映射失败（未知 PSI）直接丢帧，不向上报非法 codec。
 */
static int on_ts_packet(void *param, int program, int stream, int codecid, int flags,
                        int64_t pts_90k, int64_t dts_90k, const void *data, size_t bytes)
{
    (void)program;
    (void)stream;
    zcs_http_hls_client *c = (zcs_http_hls_client *)param;
    if (!c || c->stopping || !data || bytes == 0) {
        return 0;
    }

    zcs_codec_id codec = codecid_to_ZCS(codecid);
    if (codec == ZCS_CODEC_INVALID) {
        return 0;
    }

    if (!c->ready_sent && c->opts.on_ready) {
        c->ready_sent = 1;
        c->opts.on_ready(c->opts.user);
    }

    if (!c->opts.on_frame) {
        return 0;
    }

    zcs_frame frame;
    zcs_frame_init(&frame);
    frame.codec = codec;
    frame.track =
        (codec == ZCS_CODEC_AAC || codec == ZCS_CODEC_OPUS) ? ZCS_TRACK_AUDIO : ZCS_TRACK_VIDEO;
    frame.data = (uint8_t *)data;
    frame.size = bytes;
    frame.dts_ms = zcs_mpegts_90k_to_ms(dts_90k);
    frame.pts_ms = zcs_mpegts_90k_to_ms(pts_90k);
    frame.keyframe = (flags & MPEG_FLAG_IDR_FRAME) ? 1 : 0;
    frame.owned = 0;
    c->opts.on_frame(&frame, c->opts.user);
    return 0;
}

/* 逐 188 字节扫 TS 包。在任意偏移上找 0x47 同步字节后送入 demuxer（不假设 188
 * 对齐起点）：分片起点通常对齐，但 HTTP 拼接/前导垃圾可能让首个包偏移，扫一下
 * 更稳。末尾不足 188 的残包丢弃。 */
static zn_err_t demux_ts_segment(zcs_http_hls_client *c)
{
    if (!c->ts_demuxer || !c->body) {
        return ZN_ERR_INVALID;
    }
    if (c->body_len < 188) {
        zn_warn("hls_pull ts too short len=%u url=%s", (unsigned)c->body_len, c->fetch_url);
        return ZN_OK;
    }
    for (size_t i = 0; i + 188 <= c->body_len; i += 188) {
        if (c->body[i] != 0x47) {
            continue;
        }
        (void)ts_demuxer_input(c->ts_demuxer, c->body + i, 188);
    }
    (void)ts_demuxer_flush(c->ts_demuxer);
    return ZN_OK;
}

static uint64_t refresh_task(void *user)
{
    zcs_http_hls_client *c = (zcs_http_hls_client *)user;
    c->refresh_timer = NULL;
    if (!c || c->stopping || !c->live) {
        return 0;
    }
    zn_info("hls_pull refresh m3u8 %s", c->playlist_url);
    c->req_kind = HLS_REQ_M3U8;
    strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
    body_reset(c);
    client_close_http(c);
    start_next_segment(c);
    return 0;
}

static void hls_soft_retry(zcs_http_hls_client *c, uint64_t delay_ms)
{
    if (!c || c->stopping) {
        return;
    }
    if (!c->live) {
        c->live = 1;
    }
    cancel_refresh(c);
    client_close_http(c);
    c->http_active = 0;
    body_reset(c);
    if (delay_ms < 500) {
        delay_ms = 500;
    }
    c->refresh_timer = zn_poller_do_delay(c->opts.poller, delay_ms, refresh_task, c);
}

/*
 * 定时重拉 media playlist（live 刷新）。间隔取 target_duration/2 并夹在 [1s,10s]：
 * 半个目标时长能比上游切片快一步拿到新分片，避免边缘丢片；下限 1s 防过频请求，
 * 上限 10s 防 target 过大时刷新太懒导致延迟堆积。
 */
static void schedule_refresh(zcs_http_hls_client *c)
{
    if (!c || !c->live || c->stopping) {
        return;
    }
    cancel_refresh(c);
    uint64_t ms = (uint64_t)(c->target_duration * 500.0);
    if (ms < 1000) {
        ms = 1000;
    }
    if (ms > 10000) {
        ms = 10000;
    }
    c->refresh_timer = zn_poller_do_delay(c->opts.poller, ms, refresh_task, c);
}

/*
 * 一次 HTTP 请求结束后的主状态机推进。要点：
 *   - m3u8 拿到后若是 master，重新拉子清单；若 media 且分片为空，live 就定时重拉
 *     （上游还没切出片），VOD 才判失败。
 *   - live 且首次进入分片阶段时，seg_pos 回退到“末尾前 3 片”而非从头：从头拉
 *     会把一大段历史分片都灌给观众、延迟巨大；取最后 3 片既保证有一个关键帧起点，
 *     又贴近直播边缘。
 *   - 每片播完把 fetch_url 记进 last_seg_uri，供 start_next_segment 去重。
 */
static void on_http_done(zcs_http_hls_client *c, zn_err_t err)
{
    client_close_http(c);
    c->http_active = 0;
    if (err != ZN_OK) {
        client_fail(c, err);
        return;
    }

    if (c->pending_soft_retry) {
        c->pending_soft_retry = 0;
        hls_soft_retry(c, 1000);
        return;
    }

    if (c->req_kind == HLS_REQ_M3U8) {
        if (parse_m3u8(c, (const char *)c->body, c->body_len) != ZN_OK) {
            if (c->live) {
                zn_warn("hls_pull m3u8 parse failed, retry %s", c->playlist_url);
                hls_soft_retry(c, 1000);
                return;
            }
            client_fail(c, ZN_ERR_INVALID);
            return;
        }
        if (c->is_master_pending) {
            c->is_master_pending = 0;
            strncpy(c->playlist_url, c->fetch_url, sizeof(c->playlist_url) - 1);
            if (parse_http_url(c->fetch_url, &c->base_url) != ZN_OK) {
                client_fail(c, ZN_ERR_INVALID);
                return;
            }
            c->req_kind = HLS_REQ_M3U8;
            body_reset(c);
            start_next_segment(c);
            return;
        }
        zn_info("hls_pull m3u8 parsed: segs=%u live=%d body=%u url=%s", (unsigned)c->seg_count,
                 c->live, (unsigned)c->body_len, c->playlist_url);
        if (c->seg_count == 0) {
            if (c->live) {
                schedule_refresh(c);
                return;
            }
            client_fail(c, ZN_ERR_INVALID);
            return;
        }
        if (c->seg_pos >= c->seg_count) {
            c->seg_pos = c->live ? (c->seg_count > 3 ? c->seg_count - 3 : 0) : 0;
        }
        c->req_kind = HLS_REQ_TS;
        start_next_segment(c);
        return;
    }

    (void)demux_ts_segment(c);
    zn_info("hls_pull ts done len=%u pos=%u/%u url=%s", (unsigned)c->body_len,
             (unsigned)c->seg_pos, (unsigned)c->seg_count, c->fetch_url);
    strncpy(c->last_seg_uri, c->fetch_url, sizeof(c->last_seg_uri) - 1);
    ++c->seg_pos;
    start_next_segment(c);
}

static void hls_begin_http_fetch(zcs_http_hls_client *c, const zcs_http_hls_url *u)
{
    char ipbuf[64];
    const char *target = zcs_dns_target(u->host, ipbuf, sizeof(ipbuf));

#if ZCS_HAVE_PULL_TLS
    if (c->tls) {
        zn_err_t err = zn_tls_client_connect(c->tls, target, u->port);
        if (err != ZN_OK) {
            zn_warn("hls_pull tls connect %s:%u err=%d", u->host, (unsigned)u->port, (int)err);
        }
        if (err != ZN_OK) {
            hls_soft_retry(c, 1000);
        }
        return;
    }
#endif
    if (!c->tcp) {
        return;
    }
    zn_err_t err = zn_tcp_client_connect(c->tcp, target, u->port);
    if (err != ZN_OK) {
        zn_warn("hls_pull tcp connect %s:%u err=%d (is upstream :8080 running?)", u->host,
                 (unsigned)u->port, (int)err);
        hls_soft_retry(c, 1000);
    }
}

/*
 * 决定下一步拉什么：需要 m3u8（或无分片）就重拉清单；否则取 seg_pos 指向的分片。
 * 跳过与 last_seg_uri 相同的分片：刷新 m3u8 后新清单的第一片常是“刚播完的那片”，
 * 不去重会导致同一片被重复喂给解码器（画面卡顿/时间戳回退）。到列表末尾时 live
 * 触发定时刷新，VOD 直接停在这里。
 */
static void start_next_segment(zcs_http_hls_client *c)
{
    if (!c || c->stopping) {
        return;
    }

    if (c->req_kind == HLS_REQ_M3U8 || c->seg_count == 0) {
        c->req_kind = HLS_REQ_M3U8;
        strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
        goto do_fetch;
    }

    if (c->seg_pos >= c->seg_count) {
        if (c->live) {
            schedule_refresh(c);
        }
        return;
    }

    {
        const zcs_http_hls_segment *seg = &c->segs[c->seg_pos];
        if (strcmp(seg->uri, c->last_seg_uri) == 0) {
            ++c->seg_pos;
            if (c->seg_pos >= c->seg_count) {
                if (c->live) {
                    schedule_refresh(c);
                }
                return;
            }
            seg = &c->segs[c->seg_pos];
        }
        c->req_kind = HLS_REQ_TS;
        strncpy(c->fetch_url, seg->uri, sizeof(c->fetch_url) - 1);
    }

do_fetch: {
    zcs_http_hls_url u;
    if (parse_http_url(c->fetch_url, &u) != ZN_OK) {
        client_fail(c, ZN_ERR_INVALID);
        return;
    }
    client_close_http(c);
    body_reset(c);
    hls_begin_http_fetch(c, &u);
}
}

static void send_http_get(zcs_http_hls_client *c, const zcs_http_hls_url *u)
{
    char host_line[320];
    if ((!u->use_tls && u->port != 80) || (u->use_tls && u->port != 443)) {
        snprintf(host_line, sizeof(host_line), "%s:%u", u->host, (unsigned)u->port);
    } else {
        snprintf(host_line, sizeof(host_line), "%s", u->host);
    }

    int n = snprintf(c->req_buf, sizeof(c->req_buf),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Connection: close\r\n"
                     "User-Agent: ZCS-HlsPull/1.0\r\n"
                     "Accept: */*\r\n"
                     "\r\n",
                     u->path[0] ? u->path : "/", host_line);
    if (n > 0 && (size_t)n < sizeof(c->req_buf)) {
#if ZCS_HAVE_PULL_TLS
        if (c->tls) {
            zn_tls_client_send(c->tls, c->req_buf, (size_t)n);
            return;
        }
#endif
        if (c->tcp) {
            zn_tcp_client_send(c->tcp, c->req_buf, (size_t)n);
        }
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

static int chunk_line_complete(zcs_http_hls_chunk_dec *d, size_t *out_size)
{
    for (size_t i = 0; i < d->line_len; ++i) {
        if (d->line[i] == '\r' || d->line[i] == '\n') {
            d->line[i] = '\0';
            *out_size = (size_t)strtoul(d->line, NULL, 16);
            return 1;
        }
    }
    return 0;
}

/*
 * 响应头回调。返回值语义（message_framer 约定）：>=0 表示 Content-Length，
 * -1 表示“长度未知、按分块/直到关闭处理”，framer 据此决定 body 交付方式。
 * 非 200/206 时不立即失败而是置 pending_soft_retry：m3u8 404/503 通常是上游
 * 还没起来，段请求非 2xx 则切回重拉 m3u8（可能分片已被轮换掉）。
 * body_mode 选取优先级：chunked > Content-Length > until-close。
 */
static intptr_t on_http_header(const char *header, size_t header_len, void *user)
{
    zcs_http_hls_client *c = (zcs_http_hls_client *)user;
    if (!c) {
        return 0;
    }

    zcs_http_parser_clear(&c->http_hdr);
    if (zcs_http_parser_parse_header(&c->http_hdr, header, header_len) != ZN_OK) {
        return 0;
    }
    if (!c->http_hdr.is_response) {
        zn_warn("hls_pull not an HTTP response (first=%s)", c->http_hdr.method);
        client_fail(c, ZN_ERR_INVALID);
        return 0;
    }

    int status = atoi(c->http_hdr.url);
    if (status != 200 && status != 206) {
        if (c->req_kind == HLS_REQ_M3U8) {
            zn_warn("hls_pull m3u8 HTTP %d (wait upstream HLS) %s", status, c->fetch_url);
            c->pending_soft_retry = 1;
        } else {
            zn_warn("hls_pull segment HTTP %d %s, refresh playlist", status, c->fetch_url);
            c->pending_soft_retry = 1;
            c->req_kind = HLS_REQ_M3U8;
            strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
        }
    }

    const char *te = zcs_http_parser_header(&c->http_hdr, "Transfer-Encoding");
    const char *cl = zcs_http_parser_header(&c->http_hdr, "Content-Length");
    memset(&c->chunk, 0, sizeof(c->chunk));
    c->expect_body = 0;

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
        if (n > 0 && n <= HLS_BODY_MAX) {
            c->body_mode = BODY_CL;
            c->expect_body = (size_t)n;
            return (intptr_t)n;
        }
    }
    c->body_mode = BODY_UNTIL_CLOSE;
    return -1;
}

/*
 * body 回调。分块模式维护一个小状态机：先读 chunk 大小行（十六进制，遇 CR/LF 结束），
 * 大小为 0 即终止块，触发完成；否则按剩余字节数搬运 chunk 数据，搬完回到“读下一行”
 * 状态。非分块模式直接追加，BODY_CL 模式下长度凑够 expect_body 即完成——不依赖
 * 连接关闭，避免上游 keep-alive 迟迟不断导致分片迟迟不处理。
 */
static void on_http_content(const char *data, size_t len, void *user)
{
    zcs_http_hls_client *c = (zcs_http_hls_client *)user;
    if (!c || c->stopping) {
        return;
    }
    if (c->body_mode == BODY_CHUNKED) {
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
                        on_http_done(c, ZN_OK);
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
            if (body_append(c, (const uint8_t *)data + off, take) != ZN_OK) {
                client_fail(c, ZN_ERR_IO);
                return;
            }
            off += take;
            c->chunk.chunk_left -= take;
            if (c->chunk.chunk_left == 0) {
                c->chunk.in_chunk = 0;
            }
        }
        return;
    }
    if (body_append(c, data, len) != ZN_OK) {
        client_fail(c, ZN_ERR_IO);
        return;
    }
    if (c->body_mode == BODY_CL && c->expect_body > 0 && c->body_len >= c->expect_body) {
        on_http_done(c, ZN_OK);
    }
}

static void on_transport_data(zcs_http_hls_client *c, const void *data, size_t len)
{
    if (!c->http || !c->http_active) {
        return;
    }
    zn_err_t err = zcs_http_message_framer_input(c->http, data, len);
    if (err != ZN_OK) {
        zn_warn("hls_pull http split err=%d url=%s", (int)err, c->fetch_url);
        hls_soft_retry(c, 1000);
    }
}

static void on_io_connected(zcs_http_hls_client *c)
{
    zcs_http_hls_url u;
    if (parse_http_url(c->fetch_url, &u) != ZN_OK) {
        client_fail(c, ZN_ERR_INVALID);
        return;
    }
    c->http_active = 1;
    c->connect_retries = 0;
    zcs_http_message_framer_reset(c->http);
    body_reset(c);
    zn_info("hls_pull connected, GET %s", c->fetch_url);
    send_http_get(c, &u);
}

/* 连接被上游关闭。若声明了 Content-Length 却没收够（body 短），标记软重试——
 * 一个被截断的 TS 分片直接 demux 会丢尾部若干帧，重拉更稳。 */
static void on_io_closed(zcs_http_hls_client *c)
{
    if (!c->http_active) {
        return;
    }
    c->http_active = 0;
    if (c->body_mode == BODY_CL && c->expect_body > 0 && c->body_len < c->expect_body) {
        zn_warn("hls_pull short body %u/%u url=%s", (unsigned)c->body_len,
                 (unsigned)c->expect_body, c->fetch_url);
        c->pending_soft_retry = 1;
    }
    on_http_done(c, ZN_OK);
}

static void on_tcp_connect(zn_tcp_client *tcp, void *user)
{
    (void)tcp;
    on_io_connected((zcs_http_hls_client *)user);
}

static void on_tcp_recv(zn_tcp_client *tcp, const void *data, size_t len, void *user)
{
    (void)tcp;
    on_transport_data((zcs_http_hls_client *)user, data, len);
}

/*
 * TCP 错误。两条分路：
 *   - 已经在收 body 且已收到数据：当作“对端正常半关”，走 on_io_closed（可能完成本次）。
 *   - 否则按连接失败处理，重试且不放弃。日志做节流（前 3 次 + 每 10 次一条），
 *     ready 前后重试间隔不同（已起播 500ms 更快恢复，未起播 1000ms 别太猛）。
 */
static void on_tcp_error(zn_tcp_client *tcp, void *user)
{
    (void)tcp;
    zcs_http_hls_client *c = (zcs_http_hls_client *)user;
    if (!c || c->stopping) {
        return;
    }
    if (c->http_active && c->body_len > 0) {
        on_io_closed(c);
        return;
    }
    c->http_active = 0;
    ++c->connect_retries;
    if (c->connect_retries <= 3 || (c->connect_retries % 10) == 0) {
        zn_warn("hls_pull tcp closed/err (retry %d) ready=%d url=%s", c->connect_retries,
                 c->ready_sent, c->fetch_url);
    }
    hls_soft_retry(c, c->ready_sent ? 500 : 1000);
}

static void on_tls_connect(zn_tls_client *tls, void *user)
{
    (void)tls;
    on_io_connected((zcs_http_hls_client *)user);
}

static void on_tls_recv(zn_tls_client *tls, const void *data, size_t len, void *user)
{
    (void)tls;
    on_transport_data((zcs_http_hls_client *)user, data, len);
}

static void on_tls_error(zn_tls_client *tls, void *user)
{
    (void)tls;
    on_tcp_error(NULL, user);
}

zcs_http_hls_client *zcs_http_hls_client_create(const zcs_http_hls_client_opts *opts)
{
    if (!opts || !opts->poller || !opts->url || !opts->on_frame) {
        return NULL;
    }

    zcs_http_hls_client *c = (zcs_http_hls_client *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->opts = *opts;
    if (parse_http_url(opts->url, &c->base_url) != ZN_OK) {
        free(c);
        return NULL;
    }
    strncpy(c->playlist_url, opts->url, sizeof(c->playlist_url) - 1);

    c->ts_demuxer = ts_demuxer_create(on_ts_packet, c);
    if (!c->ts_demuxer) {
        free(c);
        return NULL;
    }

    zcs_http_message_framer_opts sopts = {on_http_header, on_http_content, c, 16U * 1024U * 1024U,
                                          NULL};
    c->http = zcs_http_message_framer_create(&sopts);
    if (!c->http) {
        ts_demuxer_destroy(c->ts_demuxer);
        free(c);
        return NULL;
    }

    if (c->base_url.use_tls) {
#if !defined(ZN_HAVE_OPENSSL) || !ZN_HAVE_OPENSSL
        zcs_http_hls_client_destroy(c);
        return NULL;
#else
        if (!opts->ssl_ctx) {
            zcs_http_hls_client_destroy(c);
            return NULL;
        }
        zn_tls_client_ops_t tops = {on_tls_connect, on_tls_recv, on_tls_error};
        zn_tls_client_opts_t topts = {opts->poller, opts->ssl_ctx, &tops, c, c->base_url.host};
        c->tls = zn_tls_client_create(&topts);
        if (!c->tls) {
            zcs_http_hls_client_destroy(c);
            return NULL;
        }
#endif
    } else {
        zn_tcp_client_ops_t tops = {on_tcp_connect, on_tcp_recv, on_tcp_error};
        zn_tcp_client_opts_t topts = {opts->poller, &tops, c};
        c->tcp = zn_tcp_client_create(&topts);
        if (!c->tcp) {
            zcs_http_hls_client_destroy(c);
            return NULL;
        }
    }
    return c;
}

void zcs_http_hls_client_destroy(zcs_http_hls_client *c)
{
    if (!c) {
        return;
    }
    zcs_http_hls_client_stop(c);
    zn_tcp_client_destroy(c->tcp);
#if ZCS_HAVE_PULL_TLS
    zn_tls_client_destroy(c->tls);
#endif
    zcs_http_message_framer_destroy(c->http);
    ts_demuxer_destroy(c->ts_demuxer);
    free(c->body);
    zcs_http_parser_clear(&c->http_hdr);
    hls_segs_clear(c);
    free(c);
}

zn_err_t zcs_http_hls_client_play(zcs_http_hls_client *c)
{
    if (!c) {
        return ZN_ERR_INVALID;
    }
    c->stopping = 0;
    c->ready_sent = 0;
    c->seg_pos = 0;
    c->last_seg_uri[0] = '\0';
    c->req_kind = HLS_REQ_M3U8;
    strncpy(c->fetch_url, c->playlist_url, sizeof(c->fetch_url) - 1);
    body_reset(c);
    c->http_active = 0;
    c->pending_soft_retry = 0;
    c->connect_retries = 0;
    /* 拉流代理按直出 HLS 处理；live 须在首次 m3u8 解析前为 1，否则 503 软重试会把 refresh_task 丢弃 */
    c->live = 1;
    zn_info("hls_pull play %s", c->playlist_url);
    start_next_segment(c);
    return ZN_OK;
}

void zcs_http_hls_client_stop(zcs_http_hls_client *c)
{
    if (!c) {
        return;
    }
    c->stopping = 1;
    cancel_refresh(c);
    client_close_http(c);
    c->http_active = 0;
    body_reset(c);
    c->ready_sent = 0;
    c->pending_soft_retry = 0;
    c->connect_retries = 0;
}
