/**
 * @file proxy.c
 * @brief 拉流代理写入本地 media_source。
 *
 * 一个拉流代理 = 一个上游客户端（RTSP/RTMP/FLV/HLS 之一）+ 一个本地
 * zcs_live_ingest。上游把收到的帧/tag 喂给 ingest，ingest 发布成本地
 * media_source，于是本地播放端（RTSP/HTTP/WebRTC…）就能把“拉来的流”
 * 当普通直播源消费。
 *
 * 并发：
 *   - 单个代理的所有回调（on_ready/on_track/on_frame/on_media/on_error）
 *     都跑在创建时 resolve 出来的那个 poller 线程上，代理字段无锁；
 *   - 跨代理的全局注册表（g_proxy_registry）是多 poller 线程共享的，
 *     访问一律持 g_proxy_registry_mtx。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/pull/proxy.h"
#include "zcs/util/base64.h"
#include "zcs/ops/config/pull_ssl.h"
#include "zn/net/ssl.h"
#include <stdio.h>
#include "zcs/core/event.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/stream/url.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "publish/rtmp/zm_ingest.h"
#include "zcs/protocol/rtsp/client.h"
#include "zcs/protocol/rtmp/client.h"
#include "zcs/protocol/http/flv_client.h"
#include "zcs/protocol/http/hls_client.h"
#include "zcs/media/wire/rtmp.h"
#include "zn/poller/poller_pool.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <stdlib.h>
#include <string.h>

/** zcs_base64_decode 的 convenience 包装：返回解出的字节数，失败返回 0。
 *  sprop-parameter-sets 解析不需要区分具体错误类型，只需长度。 */
static size_t decode_b64(const char *in, uint8_t *out, size_t cap)
{
    size_t n = 0;

    if (zcs_base64_decode(in, out, cap, &n) != ZN_OK) {
        return n; /* 缓冲不足时仍返回已解出的部分 */
    }
    return n;
}

static void trim_fmtp_token(char *s)
{
    if (!s) {
        return;
    }
    for (char *p = s; *p; ++p) {
        if (*p == ';' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            *p = '\0';
            break;
        }
    }
}

static void apply_h264_sprop(zcs_live_ingest *ch, const char *fmtp)
{
    if (!ch || !fmtp) {
        return;
    }
    /* RTSP/SDP 的 fmtp 里 sprop-parameter-sets="<SPS_b64>,<PPS_b64>" 携带带外
     * 参数集。有的上游把 SPS/PPS 只在 SDP 里给、不在码流里内联，必须解出来
     * 尽早塞进 ingest，否则下游拿不到解码参数。解析失败就静默跳过，等码流内
     * 的 IDR 自带参数集。 */
    const char *sprop = strstr(fmtp, "sprop-parameter-sets=");
    if (!sprop) {
        return;
    }
    sprop += 21;
    const char *comma = strchr(sprop, ',');
    if (!comma) {
        return;
    }

    char sps_b64[384], pps_b64[384];
    size_t slen = (size_t)(comma - sprop);
    if (slen >= sizeof(sps_b64)) {
        slen = sizeof(sps_b64) - 1;
    }
    memcpy(sps_b64, sprop, slen);
    sps_b64[slen] = '\0';
    trim_fmtp_token(sps_b64);
    strncpy(pps_b64, comma + 1, sizeof(pps_b64) - 1);
    pps_b64[sizeof(pps_b64) - 1] = '\0';
    trim_fmtp_token(pps_b64);

    uint8_t sps[256], pps[256];
    size_t sps_len = decode_b64(sps_b64, sps, sizeof(sps));
    size_t pps_len = decode_b64(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }

    if (zcs_live_ingest_set_h264_sps_pps(ch, sps, sps_len, pps, pps_len) == ZN_OK) {
        zn_debug("live_pull_proxy: applied H264 sprop SPS=%u PPS=%u", (unsigned)sps_len,
                  (unsigned)pps_len);
    }
}

static int proxy_fmtp_extract_b64(const char *fmtp, const char *key, char *out, size_t cap)
{
    char needle[32];
    const char *p;
    const char *end;
    size_t n;

    if (!fmtp || !key || !out || cap == 0) {
        return 0;
    }
    /* 在 fmtp 里找 "<key>=<value>"，value 截到下一个 ';'/空白。返回 1 表示
     * 找到且非空。用于提取 sprop-vps/sps/pps 这类单值字段。 */
    snprintf(needle, sizeof(needle), "%s=", key);
    p = strstr(fmtp, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    end = p;
    while (*end && *end != ';' && *end != ' ' && *end != '\r' && *end != '\n') {
        ++end;
    }
    n = (size_t)(end - p);
    if (n == 0 || n >= cap) {
        return 0;
    }
    memcpy(out, p, n);
    out[n] = '\0';
    trim_fmtp_token(out);
    return out[0] != '\0';
}

static void apply_h265_sprop(zcs_live_ingest *ch, const char *fmtp)
{
    char vps_b64[512], sps_b64[512], pps_b64[512];
    uint8_t vps[512], sps[512], pps[512];
    size_t vps_len = 0, sps_len = 0, pps_len = 0;

    if (!ch || !fmtp) {
        return;
    }
    /* H.265 需要 VPS/SPS/PPS 三段（VPS 可选，缺了有时仍能解）。三段都用
     * sprop-xxx=<b64> 单独给出，与 H264 的两段逗号格式不同。SPS/PPS 必需，
     * VPS 缺失时传 NULL 长度 0 交给 ingest 决定。 */
    vps_b64[0] = sps_b64[0] = pps_b64[0] = '\0';
    (void)proxy_fmtp_extract_b64(fmtp, "sprop-vps", vps_b64, sizeof(vps_b64));
    if (!proxy_fmtp_extract_b64(fmtp, "sprop-sps", sps_b64, sizeof(sps_b64))) {
        return;
    }
    if (!proxy_fmtp_extract_b64(fmtp, "sprop-pps", pps_b64, sizeof(pps_b64))) {
        return;
    }
    if (vps_b64[0]) {
        vps_len = decode_b64(vps_b64, vps, sizeof(vps));
    }
    sps_len = decode_b64(sps_b64, sps, sizeof(sps));
    pps_len = decode_b64(pps_b64, pps, sizeof(pps));
    if (!sps_len || !pps_len) {
        return;
    }
    if (zcs_live_ingest_set_h265_vps_sps_pps(ch, vps_len ? vps : NULL, vps_len, sps, sps_len, pps,
                                             pps_len) == ZN_OK) {
        zn_debug("live_pull_proxy: applied H265 sprop VPS=%u SPS=%u PPS=%u", (unsigned)vps_len,
                  (unsigned)sps_len, (unsigned)pps_len);
    }
}

static zn_err_t proxy_do_play(zcs_live_pull_proxy *p);

static void proxy_publisher_kick(void *ctx, int force)
{
    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)ctx;
    (void)force;
    if (p) {
        zcs_live_pull_proxy_stop(p);
    }
}

typedef enum {
    PROXY_RTSP = 0,
    PROXY_RTMP,
    PROXY_HTTP_FLV,
    PROXY_HLS,
} proxy_scheme;

typedef struct zcs_live_pull_proxy_ops zcs_live_pull_proxy_ops;

typedef struct zcs_live_pull_proxy_reg_entry {
    zcs_live_pull_proxy *player;
    char key[192];
    struct zcs_live_pull_proxy_reg_entry *next;
} zcs_live_pull_proxy_reg_entry;

static zcs_live_pull_proxy_reg_entry *g_proxy_registry;
static zb_mutex *g_proxy_registry_mtx;

/* 惰性初始化注册表锁：init 自身不持锁（靠“首次创建”的幂等性；代理创建通常
 * 发生在配置加载/请求处理的早期，竞态窗口极小）。注册表按 player 指针索引，
 * 用单链表头插，删除时线性查找。 */
static void proxy_registry_init(void)
{
    if (!g_proxy_registry_mtx) {
        g_proxy_registry_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    }
}

struct zcs_live_pull_proxy {
    zcs_live_pull_proxy_opts opts;
    char reg_key[192];
    char pull_url[768];
    char vhost[ZCS_VHOST_MAX];
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    zcs_live_ingest *ingress;
    void *zm_ingest;
    proxy_scheme scheme;
    const zcs_live_pull_proxy_ops *ops;
    /** 当前协议的客户端（同一时刻只有一种，类型由 ops->scheme 决定）。 */
    void *client;
    int started;
    int retry_count;
    int reconnect_delay_ms;
    int failed_count;
    int logged_rtmp_media;
    zn_poller_timer *retry_timer;
};

struct zcs_live_pull_proxy_ops {
    proxy_scheme scheme;
    const char *name;
    zn_err_t (*create)(zcs_live_pull_proxy *p, zn_poller *poller, zn_ssl_ctx *ssl_ctx);
    zn_err_t (*play)(zcs_live_pull_proxy *p);
    void (*stop)(zcs_live_pull_proxy *p);
    void (*destroy)(zcs_live_pull_proxy *p);
};

static zn_err_t proxy_rtsp_create(zcs_live_pull_proxy *p, zn_poller *poller,
                                   zn_ssl_ctx *ssl_ctx);
static zn_err_t proxy_rtmp_create(zcs_live_pull_proxy *p, zn_poller *poller,
                                   zn_ssl_ctx *ssl_ctx);
static zn_err_t proxy_http_flv_create(zcs_live_pull_proxy *p, zn_poller *poller,
                                       zn_ssl_ctx *ssl_ctx);
static zn_err_t proxy_hls_create(zcs_live_pull_proxy *p, zn_poller *poller, zn_ssl_ctx *ssl_ctx);
static zn_err_t proxy_rtsp_play(zcs_live_pull_proxy *p);
static zn_err_t proxy_rtmp_play(zcs_live_pull_proxy *p);
static zn_err_t proxy_http_flv_play(zcs_live_pull_proxy *p);
static zn_err_t proxy_hls_play(zcs_live_pull_proxy *p);
static void proxy_rtsp_stop(zcs_live_pull_proxy *p);
static void proxy_rtmp_stop(zcs_live_pull_proxy *p);
static void proxy_http_flv_stop(zcs_live_pull_proxy *p);
static void proxy_hls_stop(zcs_live_pull_proxy *p);
static void proxy_rtsp_destroy(zcs_live_pull_proxy *p);
static void proxy_rtmp_destroy(zcs_live_pull_proxy *p);
static void proxy_http_flv_destroy(zcs_live_pull_proxy *p);
static void proxy_hls_destroy(zcs_live_pull_proxy *p);

static const zcs_live_pull_proxy_ops k_proxy_pull_ops[] = {
    {PROXY_RTSP, "rtsp", proxy_rtsp_create, proxy_rtsp_play, proxy_rtsp_stop, proxy_rtsp_destroy},
    {PROXY_RTMP, "rtmp", proxy_rtmp_create, proxy_rtmp_play, proxy_rtmp_stop, proxy_rtmp_destroy},
    {PROXY_HTTP_FLV, "http-flv", proxy_http_flv_create, proxy_http_flv_play, proxy_http_flv_stop,
     proxy_http_flv_destroy},
    {PROXY_HLS, "hls", proxy_hls_create, proxy_hls_play, proxy_hls_stop, proxy_hls_destroy},
};

static const zcs_live_pull_proxy_ops *proxy_pull_ops_find(proxy_scheme scheme)
{
    size_t i;

    /* 四种上游各有一张 {create/play/stop/destroy} 函数表，按 scheme 线性查找。
     * 表驱动让 create/stop 等通用逻辑无需 switch，新增协议只需加一行。 */
    for (i = 0; i < sizeof(k_proxy_pull_ops) / sizeof(k_proxy_pull_ops[0]); ++i) {
        if (k_proxy_pull_ops[i].scheme == scheme) {
            return &k_proxy_pull_ops[i];
        }
    }
    return NULL;
}

static void proxy_cancel_retry(zcs_live_pull_proxy *p)
{
    if (p && p->retry_timer) {
        zn_poller_timer_cancel(p->retry_timer);
        p->retry_timer = NULL;
    }
}

static void strip_path_query(char *s)
{
    if (!s) {
        return;
    }
    char *q = strchr(s, '?');
    if (q) {
        *q = '\0';
    }
    char *h = strchr(s, '#');
    if (h) {
        *h = '\0';
    }
}

static const char *pull_url_path_start(const char *url)
{
    if (!url) {
        return NULL;
    }
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = strchr(p, '@');
    if (at) {
        p = at + 1;
    }
    p = strchr(p, '/');
    if (!p || !p[1]) {
        return NULL;
    }
    return p + 1;
}

static int proxy_stream_is_auto(const char *stream)
{
    /* 空串或 "auto"/"AUTO" 都表示“由 URL 自动推导流名”。 */
    if (!stream || !stream[0]) {
        return 1;
    }
    return strcmp(stream, "auto") == 0 || strcmp(stream, "AUTO") == 0;
}

int zcs_live_pull_proxy_path_tail(const char *pull_url, char *tail, size_t tail_cap)
{
    if (!tail || tail_cap == 0) {
        return -1;
    }
    tail[0] = '\0';
    /* 取 URL 路径的最后一段（去掉 app 前缀）作为候选流名：先剥 scheme/userinfo，
     * 再取最后一个 '/' 之后；最后清 query/fragment 和常见播放后缀（如 .flv），
     * 使 "rtmp://h/live/abc.flv" → "abc"。 */
    const char *p = pull_url_path_start(pull_url);
    if (!p) {
        return 0;
    }
    const char *slash = strchr(p, '/');
    if (!slash) {
        tail[0] = '\0';
        return 0;
    }
    strncpy(tail, slash + 1, tail_cap - 1);
    tail[tail_cap - 1] = '\0';
    strip_path_query(tail);
    zcs_media_path_strip_play_suffix(tail);
    return 0;
}

int zcs_live_pull_proxy_build_stream(const char *pull_url, const char *prefix, char *stream,
                                     size_t stream_cap)
{
    char tail[ZCS_STREAM_MAX];
    int n;

    if (!stream || stream_cap == 0) {
        return -1;
    }
    stream[0] = '\0';
    if (zcs_live_pull_proxy_path_tail(pull_url, tail, sizeof(tail)) != 0 || !tail[0]) {
        return -1;
    }
    if (prefix && prefix[0]) {
        n = snprintf(stream, stream_cap, "%s/%s", prefix, tail);
    } else {
        n = snprintf(stream, stream_cap, "%s", tail);
    }
    if (n < 0 || (size_t)n >= stream_cap) {
        stream[stream_cap - 1] = '\0';
        return -1;
    }
    return 0;
}

static int path_suffix_ci(const char *path, size_t len, const char *suf)
{
    size_t slen;

    if (!path || !suf || len == 0) {
        return 0;
    }
    slen = strlen(suf);
    if (len < slen) {
        return 0;
    }
#ifdef _WIN32
    return _strnicmp(path + len - slen, suf, slen) == 0;
#else
    return strncasecmp(path + len - slen, suf, slen) == 0;
#endif
}

static int is_hls_pull_url(const char *url)
{
    const char *path;
    const char *slash;
    const char *q;
    size_t n;

    if (!url) {
        return 0;
    }
    /* 只看 URL 的 path 部分（跳过 scheme 与 query）是否以 .m3u8 结尾；
     * http(s) 下面 HLS 与 HTTP-FLV 共用协议，只能靠扩展名区分。 */
    path = strstr(url, "://");
    path = path ? path + 3 : url;
    slash = strchr(path, '/');
    if (!slash) {
        return 0;
    }
    q = strchr(slash, '?');
    n = q ? (size_t)(q - slash) : strlen(slash);
    if (path_suffix_ci(slash, n, ".m3u8")) {
        return 1;
    }
    return 0;
}

static proxy_scheme detect_scheme(const char *url)
{
    if (!url) {
        return PROXY_RTSP;
    }
    /* 无法识别的一律按 RTSP 处理（rtsp:// 是本系统最常见上游）。http(s) 再按
     * 是否 .m3u8 区分 HLS / HTTP-FLV。 */
    if (strncmp(url, "rtmp://", 7) == 0 || strncmp(url, "rtmps://", 8) == 0) {
        return PROXY_RTMP;
    }
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        if (is_hls_pull_url(url)) {
            return PROXY_HLS;
        }
        return PROXY_HTTP_FLV;
    }
    return PROXY_RTSP;
}

static int pull_url_needs_tls(const char *url)
{
    return url && (strncmp(url, "rtmps://", 8) == 0 || strncmp(url, "rtsps://", 8) == 0 ||
                   strncmp(url, "https://", 8) == 0);
}

static zn_ssl_ctx *proxy_resolve_ssl_ctx(const zcs_live_pull_proxy_opts *opts)
{
    if (opts && opts->ssl_ctx) {
        return opts->ssl_ctx;
    }
    return zcs_pull_ssl_ctx(NULL);
}

#define ZCS_DEFAULT_VHOST "__defaultVhost__"

void zcs_live_pull_proxy_make_key(const char *vhost, const char *app, const char *stream, char *key,
                                  size_t key_cap)
{
    if (!key || key_cap == 0) {
        return;
    }
    const char *vh = (vhost && vhost[0]) ? vhost : ZCS_DEFAULT_VHOST;
    /* 注册键 = vhost/app/stream；vhost 缺省用 "__defaultVhost__" 占位，
     * 保证“未指定 vhost”与“显式指定默认 vhost”落到同一个键上。 */
    snprintf(key, key_cap, "%s/%s/%s", vh, app ? app : "", stream ? stream : "");
}

static void proxy_registry_add(zcs_live_pull_proxy *p)
{
    if (!p || !p->reg_key[0]) {
        return;
    }
    proxy_registry_init();
    zcs_live_pull_proxy_reg_entry *e = (zcs_live_pull_proxy_reg_entry *)calloc(1, sizeof(*e));
    if (!e) {
        return;
    }
    e->player = p;
    strncpy(e->key, p->reg_key, sizeof(e->key) - 1);
    /* 头插 + 持锁：注册表被多 poller 线程并发读（find/foreach），插入时必须
     * 串行，否则读者可能看到半链接的节点。 */
    zb_mutex_lock(g_proxy_registry_mtx);
    e->next = g_proxy_registry;
    g_proxy_registry = e;
    zb_mutex_unlock(g_proxy_registry_mtx);
}

static void proxy_registry_remove(zcs_live_pull_proxy *p)
{
    zcs_live_pull_proxy_reg_entry **pp;

    if (!p) {
        return;
    }
    /* 按 player 指针摘除（而非 key）：同一 key 理论上可能被重建，按指针
     * 删才精确。用二级指针遍历法，命中的是链表里的节点本身。 */
    proxy_registry_init();
    zb_mutex_lock(g_proxy_registry_mtx);
    pp = &g_proxy_registry;
    while (*pp) {
        if ((*pp)->player == p) {
            zcs_live_pull_proxy_reg_entry *dead = *pp;
            *pp = dead->next;
            zb_mutex_unlock(g_proxy_registry_mtx);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
    zb_mutex_unlock(g_proxy_registry_mtx);
}

zcs_live_pull_proxy *zcs_live_pull_proxy_find_by_key(const char *key)
{
    zcs_live_pull_proxy *found = NULL;

    if (!key) {
        return NULL;
    }
    proxy_registry_init();
    zb_mutex_lock(g_proxy_registry_mtx);
    for (zcs_live_pull_proxy_reg_entry *e = g_proxy_registry; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            found = e->player;
            break;
        }
    }
    zb_mutex_unlock(g_proxy_registry_mtx);
    return found;
}

int zcs_live_pull_proxy_foreach(zcs_live_pull_proxy_visit_cb cb, void *user)
{
    int n = 0;

    if (!cb) {
        return 0;
    }
    proxy_registry_init();
    zb_mutex_lock(g_proxy_registry_mtx);
    for (zcs_live_pull_proxy_reg_entry *e = g_proxy_registry; e; e = e->next) {
        if (cb(e->key, e->player, user) != 0) {
            break;
        }
        ++n;
    }
    zb_mutex_unlock(g_proxy_registry_mtx);
    return n;
}

static zn_poller *resolve_poller(const zcs_live_pull_proxy_opts *opts)
{
    /* 优先用调用方明确给的 poller；否则从 poller 池取 0 号。把代理钉在固定
     * poller 上，保证该代理的所有回调都在同一线程——这正是内部无锁的前提。 */
    if (opts->poller) {
        return opts->poller;
    }
    if (opts->poller_pool) {
        return zn_poller_pool_get(opts->poller_pool, 0);
    }
    return NULL;
}

static void proxy_try_publish(zcs_live_pull_proxy *p)
{
    zcs_media_source *src;

    if (!p || !p->ingress) {
        return;
    }
    src = zcs_live_ingest_source(p->ingress);
    if (!src || src->publishing) {
        return;
    }
    /* 必须等视频轨 ready 才 publish：只有音频就开播会让播放端拿到一个“有
     * 声音没画面”的源，且首个观众会卡在等关键帧。视频 ready 表示已拿到
     * SPS/PPS 能与可解码的 IDR，此时发布最稳。 */
    if (!src->has_video || !src->video.ready) {
        return;
    }
    zn_debug("live_pull_proxy publish: %s/%s tracks ready (video=%d audio=%d)", p->app, p->stream,
              src->has_video, src->has_audio);
    zcs_media_event_publish(src, ZCS_ORIGIN_PULL);
}

static void proxy_on_ready(void *user)
{
    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)user;
    /* 连上并协商完成：清失败计数、取消重试定时器，并把自己登记为该源的
     * “publisher”。设置 publisher 回调（kick）：当有别的发布者抢占用同一
     * 路流时，源会回调它来踢掉本代理。 */
    p->failed_count = 0;
    proxy_cancel_retry(p);
    zn_info("live_pull_proxy ready: %s/%s from %s", p->app, p->stream, p->pull_url);
    {
        zcs_media_source *src = zcs_live_ingest_source(p->ingress);
        if (src) {
            zcs_media_source_set_publisher(src, p, proxy_publisher_kick);
        }
    }
    proxy_try_publish(p);
    if (p->opts.on_ready) {
        p->opts.on_ready(p->opts.user);
    }
}

static void proxy_on_track(const zcs_media_track *track, void *user)
{
    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)user;
    if (!p || !p->ingress || !track) {
        return;
    }
    /* 每次拿到一条轨的 SDP 信息就尽量抽取带内/带外解码参数：H264/H265 取
     * sprop 参数集；AAC 取 config（AudioSpecificConfig）hex；G711 确定采样率
     * 并设置 RTP 时钟（8000Hz 典型）。这些参数决定下游能否解出声音/画面。 */
    if (track->codec == ZCS_CODEC_H264 && track->fmtp[0]) {
        apply_h264_sprop(p->ingress, track->fmtp);
    } else if (track->codec == ZCS_CODEC_H265 && track->fmtp[0]) {
        apply_h265_sprop(p->ingress, track->fmtp);
    }
    if (track->codec == ZCS_CODEC_AAC && track->fmtp[0]) {
        const char *cfg = strstr(track->fmtp, "config=");
        if (!cfg) {
            cfg = strstr(track->fmtp, "Config=");
        }
        if (cfg) {
            char hex[128];
            strncpy(hex, cfg + 7, sizeof(hex) - 1);
            hex[sizeof(hex) - 1] = '\0';
            trim_fmtp_token(hex);
            if (hex[0]) {
                (void)zcs_live_ingest_set_aac_config_hex(p->ingress, hex);
                zcs_live_ingest_mark_real_audio(p->ingress);
            }
        }
    }
    if (track->type == ZCS_TRACK_AUDIO &&
        (track->codec == ZCS_CODEC_G711A || track->codec == ZCS_CODEC_G711U)) {
        int rate = track->sample_rate > 0 ? track->sample_rate : 8000;
        zcs_live_ingest_set_audio_codec(p->ingress, track->codec, (uint32_t)rate);
        zcs_live_ingest_set_rtp_clocks(p->ingress, 90000, track->codec, (uint32_t)rate);
    }
    proxy_try_publish(p);
}

static void proxy_on_frame(const zcs_frame *frame, void *user)
{
    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)user;
    if (!p || !p->ingress || !frame) {
        return;
    }
    /* RTSP/HLS 上游给的是已解好的帧（ES），直接喂 ingest；ingest 内部会攒
     * config、判关键帧、写 gop_queue。每帧后尝试 publish，覆盖“音视频参数
     * 晚于首帧拿到”的情形。 */
    (void)zcs_live_ingest_input_frame(p->ingress, frame);
    proxy_try_publish(p);
}

static void proxy_on_rtmp_media(uint8_t msg_type, uint32_t tag_dts_ms, const void *body, size_t len,
                                void *user)
{
    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)user;
    if (!p || !p->ingress || !body || len == 0) {
        return;
    }
    if (!p->logged_rtmp_media) {
        p->logged_rtmp_media = 1;
        zn_debug("live_pull_proxy first RTMP media type=%u len=%u", (unsigned)msg_type,
                  (unsigned)len);
    }
    /* RTMP/FLV 上游给的是封装好的 tag（含 FLV 视频/音频头），需要 zm ingest
     * 负责解封装成帧。zm_ingest 惰性创建，只在收到第一个音视频 tag 时建。
     * 非音视频 tag（脚本/命令）忽略。 */
    if (msg_type == ZCS_RTMP_MSG_VIDEO || msg_type == ZCS_RTMP_MSG_AUDIO) {
        if (!p->zm_ingest) {
            p->zm_ingest = zcs_rtmp_zm_ingest_create(p->ingress);
        }
        if (p->zm_ingest) {
            (void)zcs_rtmp_zm_ingest_input_tag((zcs_rtmp_zm_ingest *)p->zm_ingest, msg_type,
                                               body, len, tag_dts_ms);
        }
    }
    proxy_try_publish(p);
}

static uint64_t proxy_retry_task(void *user)
{
    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)user;
    p->retry_timer = NULL;
    if (!p || !p->started) {
        return 0;
    }
    zn_info("live_pull_proxy retry #%d pull=%s", p->failed_count + 1, p->pull_url);
    /* 重连前复位上游状态：清掉 ingest 里上一个连接的序列/时间戳基准，避免
     * 重连后时间戳跳变；清 logged_rtmp_media 让新连接的首包日志再打一次。
     * play 仍失败则返回延迟让定时器自排下一次重试。 */
    if (p->ingress) {
        zcs_live_ingest_reset_upstream(p->ingress);
    }
    p->logged_rtmp_media = 0;
    if (proxy_do_play(p) != ZN_OK) {
        return (uint64_t)p->reconnect_delay_ms;
    }
    return 0;
}

static void proxy_schedule_retry(zcs_live_pull_proxy *p)
{
    if (!p || !p->started) {
        return;
    }
    /* retry_count<0 表示无限重试；>=0 表示达到次数上限后彻底停。注意这里是
     * “已失败次数 >= 上限”才放弃，且失败计数在排程时自增。 */
    if (p->retry_count >= 0 && p->failed_count >= p->retry_count) {
        return;
    }
    ++p->failed_count;
    proxy_cancel_retry(p);
    p->retry_timer =
        zn_poller_do_delay(p->opts.poller, (uint64_t)p->reconnect_delay_ms, proxy_retry_task, p);
}

static void proxy_on_error(zn_err_t err, void *user)
{
    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)user;
    zn_warn("live_pull_proxy error: %s/%s err=%d", p->app, p->stream, (int)err);
    /* 出错先停下当前协议客户端（避免它在坏连接上继续收），再通知上层，
     * 最后按退避排重连。顺序保证上层看到的错误发生时资源已收拢。 */
    if (p->ops && p->ops->stop) {
        p->ops->stop(p);
    }
    if (p->opts.on_error) {
        p->opts.on_error(err, p->opts.user);
    }
    proxy_schedule_retry(p);
}

static zn_err_t proxy_do_play(zcs_live_pull_proxy *p)
{
    if (!p) {
        return ZN_ERR_INVALID;
    }
    return p->ops && p->ops->play ? p->ops->play(p) : ZN_ERR_INVALID;
}

static zn_err_t proxy_rtsp_play(zcs_live_pull_proxy *p)
{
    return p && ((zcs_rtsp_client *)p->client) ? zcs_rtsp_client_play(((zcs_rtsp_client *)p->client)) : ZN_ERR_INVALID;
}

static zn_err_t proxy_rtmp_play(zcs_live_pull_proxy *p)
{
    return p && ((zcs_rtmp_client *)p->client) ? zcs_rtmp_client_play(((zcs_rtmp_client *)p->client)) : ZN_ERR_INVALID;
}

static zn_err_t proxy_http_flv_play(zcs_live_pull_proxy *p)
{
    return p && ((zcs_http_flv_client *)p->client) ? zcs_http_flv_client_play(((zcs_http_flv_client *)p->client)) : ZN_ERR_INVALID;
}

static zn_err_t proxy_hls_play(zcs_live_pull_proxy *p)
{
    if (!p || !((zcs_http_hls_client *)p->client)) {
        return ZN_ERR_INVALID;
    }
    (void)zcs_http_hls_client_play(((zcs_http_hls_client *)p->client));
    return ZN_OK;
}

static void proxy_rtsp_stop(zcs_live_pull_proxy *p)
{
    if (p && ((zcs_rtsp_client *)p->client)) {
        zcs_rtsp_client_stop(((zcs_rtsp_client *)p->client));
    }
}

static void proxy_rtmp_stop(zcs_live_pull_proxy *p)
{
    if (p && ((zcs_rtmp_client *)p->client)) {
        zcs_rtmp_client_stop(((zcs_rtmp_client *)p->client));
    }
}

static void proxy_http_flv_stop(zcs_live_pull_proxy *p)
{
    if (p && ((zcs_http_flv_client *)p->client)) {
        zcs_http_flv_client_stop(((zcs_http_flv_client *)p->client));
    }
}

static void proxy_hls_stop(zcs_live_pull_proxy *p)
{
    if (p && ((zcs_http_hls_client *)p->client)) {
        zcs_http_hls_client_stop(((zcs_http_hls_client *)p->client));
    }
}

static void proxy_rtsp_destroy(zcs_live_pull_proxy *p)
{
    if (p) {
        zcs_rtsp_client_destroy(((zcs_rtsp_client *)p->client));
    }
}

static void proxy_rtmp_destroy(zcs_live_pull_proxy *p)
{
    if (p) {
        zcs_rtmp_client_destroy(((zcs_rtmp_client *)p->client));
        if (p->zm_ingest) {
            zcs_rtmp_zm_ingest_destroy((zcs_rtmp_zm_ingest *)p->zm_ingest);
            p->zm_ingest = NULL;
        }
    }
}

static void proxy_http_flv_destroy(zcs_live_pull_proxy *p)
{
    if (p) {
        zcs_http_flv_client_destroy(((zcs_http_flv_client *)p->client));
    }
}

static void proxy_hls_destroy(zcs_live_pull_proxy *p)
{
    if (p) {
        zcs_http_hls_client_destroy(((zcs_http_hls_client *)p->client));
    }
}

static zn_err_t proxy_rtsp_create(zcs_live_pull_proxy *p, zn_poller *poller, zn_ssl_ctx *ssl_ctx)
{
    zcs_rtsp_client_opts copts;

    if (!p) {
        return ZN_ERR_INVALID;
    }
    /* 四种 create 结构一致：填 opts（poller/url/ssl + 一组回调）→ 创建客户端。
     * 客户端自身的重连逻辑关掉（retry_count=0 已归一为无限，这里显式置 0 抑制
     * 内层重连），因为重连由本代理统一调度，避免两层重试叠加。 */
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.rtp_mode = p->opts.rtp_mode;
    copts.retry_count = 0;
    copts.reconnect_delay_ms = 0;
    copts.on_ready = proxy_on_ready;
    copts.on_track = proxy_on_track;
    copts.on_frame = proxy_on_frame;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->client = (void *)zcs_rtsp_client_create(&copts);
    return p->client ? ZN_OK : ZN_ERR_NOMEM;
}

static zn_err_t proxy_rtmp_create(zcs_live_pull_proxy *p, zn_poller *poller, zn_ssl_ctx *ssl_ctx)
{
    zcs_rtmp_client_opts copts;

    if (!p) {
        return ZN_ERR_INVALID;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.on_ready = proxy_on_ready;
    copts.on_media = proxy_on_rtmp_media;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->client = (void *)zcs_rtmp_client_create(&copts);
    return p->client ? ZN_OK : ZN_ERR_NOMEM;
}

static zn_err_t proxy_http_flv_create(zcs_live_pull_proxy *p, zn_poller *poller,
                                       zn_ssl_ctx *ssl_ctx)
{
    zcs_http_flv_client_opts copts;

    if (!p) {
        return ZN_ERR_INVALID;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.on_ready = proxy_on_ready;
    copts.on_media = proxy_on_rtmp_media;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->client = (void *)zcs_http_flv_client_create(&copts);
    return p->client ? ZN_OK : ZN_ERR_NOMEM;
}

static zn_err_t proxy_hls_create(zcs_live_pull_proxy *p, zn_poller *poller, zn_ssl_ctx *ssl_ctx)
{
    zcs_http_hls_client_opts copts;

    if (!p) {
        return ZN_ERR_INVALID;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.url = p->pull_url;
    copts.ssl_ctx = ssl_ctx;
    copts.on_ready = proxy_on_ready;
    copts.on_frame = proxy_on_frame;
    copts.on_error = proxy_on_error;
    copts.user = p;
    p->client = (void *)zcs_http_hls_client_create(&copts);
    return p->client ? ZN_OK : ZN_ERR_NOMEM;
}

zcs_live_pull_proxy *zcs_live_pull_proxy_create(const zcs_live_pull_proxy_opts *opts)
{
    if (!opts || !opts->pull_url) {
        return NULL;
    }
    zn_poller *poller = resolve_poller(opts);
    if (!poller) {
        return NULL;
    }

    zcs_live_pull_proxy *p = (zcs_live_pull_proxy *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->opts = *opts;
    p->opts.poller = poller;
    zcs_vhost_normalize(opts->vhost, p->vhost, sizeof(p->vhost));
    strncpy(p->pull_url, opts->pull_url, sizeof(p->pull_url) - 1);
    p->scheme = detect_scheme(p->pull_url);
    p->ops = proxy_pull_ops_find(p->scheme);
    if (!p->ops) {
        free(p);
        return NULL;
    }

    if (opts->app && opts->app[0]) {
        strncpy(p->app, opts->app, sizeof(p->app) - 1);
    } else {
        strncpy(p->app, "live", sizeof(p->app) - 1);
    }

    if (!proxy_stream_is_auto(opts->stream) && opts->stream && opts->stream[0]) {
        strncpy(p->stream, opts->stream, sizeof(p->stream) - 1);
    } else if (zcs_live_pull_proxy_build_stream(p->pull_url, opts->proxy_prefix, p->stream,
                                                sizeof(p->stream)) != 0 ||
               !p->stream[0]) {
        /* 未显式给流名（或给了 "auto"）：从 URL 路径尾部自动推导。推导失败
         * 就没法把流挂到命名空间，直接创建失败——比造一个空流名更安全。 */
        zn_error("live_pull_proxy: derive stream name failed pull=%s", p->pull_url);
        free(p);
        return NULL;
    }
    zn_debug("live_pull_proxy register %s/%s pull=%s", p->app, p->stream, p->pull_url);

    p->retry_count = opts->retry_count;
    /* 同 RTSP 客户端约定：0 视为“未设置”→ 无限重试(-1)；否则用给定次数。 */
    if (p->retry_count == 0) {
        p->retry_count = -1;
    }
    p->reconnect_delay_ms = opts->reconnect_delay_ms > 0 ? opts->reconnect_delay_ms : 2000;

    p->ingress = zcs_live_ingest_create_vhost(p->vhost, p->app, p->stream, NULL);
    if (!p->ingress) {
        free(p);
        return NULL;
    }
    zcs_live_ingest_set_poller(p->ingress, poller);
    {
        /* 把 URL 尾部记到 src->stream_requested：播放端请求相对路径（如
         * "abc.flv"）时，源可用它做别名匹配，兼容不同命名。 */
        zcs_media_source *src = zcs_live_ingest_source(p->ingress);
        char tail[ZCS_STREAM_MAX];
        if (src && zcs_live_pull_proxy_path_tail(p->pull_url, tail, sizeof(tail)) == 0 && tail[0]) {
            strncpy(src->stream_requested, tail, sizeof(src->stream_requested) - 1);
            src->stream_requested[sizeof(src->stream_requested) - 1] = '\0';
        }
    }

    zn_ssl_ctx *ssl_ctx = pull_url_needs_tls(p->pull_url) ? proxy_resolve_ssl_ctx(opts) : NULL;
    /* TLS 上游拿不到 SSL 上下文就直接失败：不能悄悄退回明文去连 TLS 端口。 */
    if (pull_url_needs_tls(p->pull_url) && !ssl_ctx) {
        zcs_live_ingest_destroy(p->ingress);
        free(p);
        return NULL;
    }

    if (p->ops->create(p, poller, ssl_ctx) != ZN_OK) {
        zcs_live_ingest_destroy(p->ingress);
        free(p);
        return NULL;
    }
    zcs_live_pull_proxy_make_key(p->vhost, p->app, p->stream, p->reg_key, sizeof(p->reg_key));
    proxy_registry_add(p);
    return p;
}

void zcs_live_pull_proxy_destroy(zcs_live_pull_proxy *p)
{
    if (!p) {
        return;
    }
    /* 销毁顺序：先摘注册表（防止并发 find 拿到将死的指针）→ 停定时器 → 停
     * 客户端（发 fini、断上游）→ 销毁协议客户端 → 销毁 ingest → 释放。 */
    proxy_registry_remove(p);
    proxy_cancel_retry(p);
    zcs_live_pull_proxy_stop(p);
    if (p->ops && p->ops->destroy) {
        p->ops->destroy(p);
    }
    if (p->zm_ingest) {
        zcs_rtmp_zm_ingest_destroy((zcs_rtmp_zm_ingest *)p->zm_ingest);
        p->zm_ingest = NULL;
    }
    zcs_live_ingest_destroy(p->ingress);
    free(p);
}

zn_err_t zcs_live_pull_proxy_start(zcs_live_pull_proxy *p)
{
    if (!p) {
        return ZN_ERR_INVALID;
    }
    /* 幂等启动：已在跑直接返回成功。started 置位后才真正发起 play，因此
     * 若 play 过程中同步触发了 error 回调，proxy_schedule_retry 能看到
     * started=1 而正常排重试。 */
    if (p->started) {
        return ZN_OK;
    }
    zn_debug("live_pull_proxy start pull=%s -> %s/%s", p->pull_url, p->app, p->stream);
    p->started = 1;
    p->failed_count = 0;
    return proxy_do_play(p);
}

void zcs_live_pull_proxy_stop(zcs_live_pull_proxy *p)
{
    if (!p || !p->started) {
        return;
    }
    p->started = 0;
    proxy_cancel_retry(p);
    /* 先从源上摘掉 publisher 身份（避免 kick 回调再打到已停的代理），若源
     * 处于 publishing 则发 publish_fini 事件通知订阅者流已结束，再停客户端、
     * 复位 ingest。顺序保证在停网络 IO 前先让下游感知到下播。 */
    {
        zcs_media_source *src = zcs_live_ingest_source(p->ingress);
        if (src) {
            zcs_media_source_clear_publisher(src, p);
        }
        if (src && src->publishing) {
            zcs_media_event_publish_fini(src, ZCS_ORIGIN_PULL);
        }
    }
    if (p->ops && p->ops->stop) {
        p->ops->stop(p);
    }
    if (p->ingress) {
        zcs_live_ingest_reset(p->ingress);
    }
}

zcs_media_source *zcs_live_pull_proxy_source(zcs_live_pull_proxy *p)
{
    return p ? zcs_live_ingest_source(p->ingress) : NULL;
}

zcs_live_ingest *zcs_live_pull_proxy_ingress(zcs_live_pull_proxy *p)
{
    return p ? p->ingress : NULL;
}

const char *zcs_live_pull_proxy_app(const zcs_live_pull_proxy *p)
{
    return p ? p->app : "";
}

const char *zcs_live_pull_proxy_stream(const zcs_live_pull_proxy *p)
{
    return p ? p->stream : "";
}
