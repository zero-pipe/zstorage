/**
 * @file hub.c
 * @brief 流注册表、推流/播放读者挂接与生命周期。
 *
 * 并发模型：注册表是一张单向链表 g_head，所有增删查都由 g_registry_mtx 保护
 * （zcs_media_source_registry_lock/unlock）。但**源对象本身**（zcs_media_source）
 * 的媒体数据字段由各自推流/播放线程访问，本文件只在持锁时做链表层面的操作。
 * 因此约定：调用方拿到返回的 source 指针后，链表增删可能让该节点被 free——
 * 但 ZCS 的用法是 source 在 unregister 前一直有效，读者在 detach 时自行注销。
 * 节点本身会被 free（unregister）或复用（同名重注册），所以**是否有野指针**
 * 取决于 reader/clear 的同步：gop_queue_clear 走 retire 延迟回收（读者在无锁
 * 临界区内持有的 slot 不会在脚下被释放），重注册复用节点保证 reader 的 source
 * 指针不悬空。见 src/core/gop/queue.c 的 clear/retire 与 test_gop_clear_race。
 *
 * 为什么用侵入式链表而非数组：源的数量少（几十路）、增删频繁（推流上下线），
 * 链表 O(1) 插入 + 稳定指针（节点地址即 source 地址）最省心，也便于直接
 * 把 &node->src 作为对外句柄返回。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/stream/hub.h"
#include "zcs/core/event.h"
#include "zcs/core/stream/stats.h"
#include "zcs/media/media_limits.h"
#include "zcs/core/stream/url.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/core/stream/buffer.h"
#include "zcs/core/stream/vod_source_port.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include "zn/platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct zcs_source_node {
    zcs_media_source src;
    struct zcs_source_node *next;
} zcs_source_node;

static zcs_source_node *g_head;
static int g_stream_hub_initialized;
static zb_mutex *g_registry_mtx;

/* “源是否已在提供服务”：有 gop_queue（直播已挂接）或有 vod_buffer（点播已打开）。
 * 仅登记了 key 但尚未起播/无底层缓冲的节点不算 active，避免被 API / ONVIF 当成可播放。 */
static int source_active(const zcs_media_source *s)
{
    return s && (s->gop_queue || s->vod_buffer);
}

static int g_vhost_enabled;

void zcs_vhost_set_enabled(int enable)
{
    g_vhost_enabled = enable ? 1 : 0;
}

int zcs_vhost_enabled(void)
{
    return g_vhost_enabled;
}

/*
 * 把各种形态的 host 输入归一成 vhost key。输入可能来自 SNI、Host 头、
 * RTSP URL 主机、带端口的 "[ipv6]:port"、甚至整条 URL——这里统一：
 * 去掉 scheme、userinfo 前的 '@' 之外的逻辑（本函数不处理 userinfo）、
 * 端口、路径、查询串；转小写；去掉末尾的点（FQDN 尾点）。
 * 归一化保证 "Cam.Local" 与 "cam.local:8080" 命中同一源。
 * g_vhost_enabled 关闭时一律归到默认 vhost，等价于“不区分虚拟主机”。
 */
void zcs_vhost_normalize(const char *in, char *out, size_t cap)
{
    const char *p;
    size_t n = 0;

    if (!out || cap == 0) {
        return;
    }
    if (!g_vhost_enabled || !in) {
        strncpy(out, ZCS_DEFAULT_VHOST, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    p = in;
    while (*p == ' ' || *p == '	') {
        ++p;
    }
    {
        const char *scheme = strstr(p, "://");
        if (scheme) {
            p = scheme + 3;
        }
    }
    if (*p == '[') { /* [ipv6]:port */
        ++p;
        while (*p && *p != ']' && n + 1 < cap) {
            out[n++] = (char)tolower((unsigned char)*p++);
        }
    } else {
        while (*p && *p != ':' && *p != '/' && *p != '?' && *p != ',' && *p != ' ' &&
               *p != '	' && n + 1 < cap) {
            out[n++] = (char)tolower((unsigned char)*p++);
        }
    }
    while (n > 0 && out[n - 1] == '.') {
        --n;
    }
    out[n] = 0;
    if (n == 0) {
        strncpy(out, ZCS_DEFAULT_VHOST, cap - 1);
        out[cap - 1] = 0;
    }
}

/* 分配并头插一个源节点（调用方须持 g_registry_mtx）。
 * 头插让最近的源排在最前，find 时通常很快命中；节点地址即 &node->src，
 * 直接作为对外句柄，无需额外映射。 */
static zcs_media_source *source_node_alloc(void)
{
    zcs_source_node *n = (zcs_source_node *)calloc(1, sizeof(*n));

    if (!n) {
        return NULL;
    }
    n->next = g_head;
    g_head = n;
    return &n->src;
}

static void source_node_free(zcs_media_source *s)
{
    zcs_source_node **pp = &g_head;

    if (!s) {
        return;
    }
    /* 丢弃不透明分片 sidecar 袋（录制器应已销毁）。 */
    if (s->segment_sidecar) {
        free(s->segment_sidecar);
        s->segment_sidecar = NULL;
    }
    while (*pp) {
        if (&(*pp)->src == s) {
            zcs_source_node *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

static int registry_count_active(void)
{
    int n = 0;

    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (source_active(&node->src)) {
            ++n;
        }
    }
    return n;
}

static int registry_count_live(void)
{
    int n = 0;

    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (node->src.gop_queue) {
            ++n;
        }
    }
    return n;
}

void zcs_media_source_registry_lock(void)
{
    zcs_media_source_registry_init();
    zb_mutex_lock(g_registry_mtx);
}

void zcs_media_source_registry_unlock(void)
{
    if (g_registry_mtx) {
        zb_mutex_unlock(g_registry_mtx);
    }
}

void zcs_media_source_registry_init(void)
{
    if (g_stream_hub_initialized) {
        return;
    }
    g_head = NULL;
    g_registry_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    g_stream_hub_initialized = 1;
}

static void strip_query(char *s)
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

/*
 * 从任意路径/URL 中抽出 (app, stream) 两段。
 * 支持的输入：裸路径 "/app/stream"、完整 URL "rtsp://user@host/app/stream"、
 * 只有一段 "app"（此时 stream 留空）。URL 形态下先跳过 scheme、userinfo 和主机名，
 * 再按第一个 '/' 切 app/stream。查询串与 fragment 一律丢弃，避免 "stream?x=1"。
 */
void zcs_media_split_path(const char *path_or_url, char *app, char *stream)
{
    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path_or_url || !path_or_url[0] || !app || !stream) {
        return;
    }

    const char *seg = path_or_url;
    const char *scheme = strstr(path_or_url, "://");
    if (scheme) {
        seg = scheme + 3;
        const char *at = strchr(seg, '@');
        if (at) {
            seg = at + 1;
        }
        seg = strchr(seg, '/');
        if (!seg || !seg[1]) {
            return;
        }
        seg++;
    } else if (seg[0] == '/') {
        seg++;
    }

    const char *slash = strchr(seg, '/');
    if (!slash) {
        strncpy(app, seg, ZCS_APP_MAX - 1);
        app[ZCS_APP_MAX - 1] = '\0';
        strip_query(app);
        return;
    }
    size_t alen = (size_t)(slash - seg);
    if (alen >= ZCS_APP_MAX) {
        alen = ZCS_APP_MAX - 1;
    }
    memcpy(app, seg, alen);
    app[alen] = '\0';
    strncpy(stream, slash + 1, ZCS_STREAM_MAX - 1);
    stream[ZCS_STREAM_MAX - 1] = '\0';
    strip_query(stream);
}

static int key_match(const zcs_media_source *s, const char *schema, const char *vhost,
                     const char *app, const char *stream)
{
    return source_active(s) && strcmp(s->schema, schema) == 0 &&
           strcmp(s->vhost, vhost) == 0 && strcmp(s->app, app) == 0 && strcmp(s->stream, stream) == 0;
}

int zcs_media_source_count(void)
{
    int n;

    zcs_media_source_registry_lock();
    n = registry_count_active();
    zcs_media_source_registry_unlock();
    return n;
}

void zcs_media_source_log_registry(const zcs_media_server_ports *ports)
{
    zcs_media_source_registry_lock();
    int n = registry_count_active();
    if (!ports) {
        zn_debug("[source] registry_dump active=%d", n);
        zcs_media_source_registry_unlock();
        return;
    }
    zn_debug("[source] registry_dump active=%d rtmp=%u rtsp=%u http=%u", n, ports->rtmp,
              ports->rtsp, ports->http);
    if (n == 0) {
        zn_debug("[source] registry_empty hint=push_rtmp_or_rtsp_record_before_play");
        zn_debug("[source] registry_empty hint=check_duplicate_demo_or_log_file");
        zcs_media_source_registry_unlock();
        return;
    }
    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (source_active(&node->src)) {
            zcs_media_urls_log_stream(&node->src, ports);
        }
    }
    zcs_media_source_registry_unlock();
}

zcs_media_source *zcs_media_source_find_ex(const char *schema, const char *vhost, const char *app,
                                           const char *stream)
{
    char vh[ZCS_VHOST_MAX];

    if (!schema || !app || !stream) {
        return NULL;
    }
    zcs_vhost_normalize(vhost, vh, sizeof(vh));
    zcs_media_source_registry_lock();
    zcs_media_source *found = NULL;
    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, vh, app, stream)) {
            found = &node->src;
            break;
        }
    }
    zcs_media_source_registry_unlock();
    return found;
}

zcs_media_source *zcs_media_source_find(const char *schema, const char *app, const char *stream)
{
    return zcs_media_source_find_ex(schema, ZCS_DEFAULT_VHOST, app, stream);
}

static zcs_media_source *find_by_stream_requested(const char *schema, const char *vhost,
                                                  const char *app, const char *name)
{
    if (!schema || !vhost || !app || !name || !name[0]) {
        return NULL;
    }
    zcs_media_source_registry_lock();
    zcs_media_source *found = NULL;
    for (zcs_source_node *node = g_head; node; node = node->next) {
        const zcs_media_source *s = &node->src;
        if (!s->gop_queue || strcmp(s->schema, schema) != 0 ||
            strcmp(s->vhost, vhost) != 0 || strcmp(s->app, app) != 0) {
            continue;
        }
        if (strcmp(s->stream_requested, name) == 0 || strcmp(s->stream, name) == 0) {
            found = &node->src;
            break;
        }
    }
    zcs_media_source_registry_unlock();
    return found;
}

static int stream_ends_with_id(const char *stream, const char *id)
{
    if (!stream || !id || !id[0]) {
        return 0;
    }
    size_t slen = strlen(stream);
    size_t id_len = strlen(id);
    if (slen < id_len) {
        return 0;
    }
    if (strcmp(stream + slen - id_len, id) != 0) {
        return 0;
    }
    return slen == id_len || stream[slen - id_len - 1] == '/';
}

/** demo/VLC live/stream；仅一条流时解析为 live 流 */
/* 只有在 app=="live" 且 stream=="stream" 且该 app 下恰好只有一路 live 时才命中。
 * “恰好唯一”是硬条件：VLC/demo 常用 /live/stream 这种泛化入口，若有多路就无法
 * 判断用户想要哪一路，宁可返回 NULL 让上层报错，也不随便挑一路造成串流。 */
static zcs_media_source *find_single_live_stream_alias(const char *vhost, const char *app,
                                                       const char *stream)
{
    if (!vhost || !app || strcmp(app, "live") != 0 || !stream || strcmp(stream, "stream") != 0) {
        return NULL;
    }
    zcs_media_source_registry_lock();
    zcs_media_source *found = NULL;
    int n = 0;
    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (!node->src.gop_queue || strcmp(node->src.vhost, vhost) != 0 ||
            strcmp(node->src.app, app) != 0) {
            continue;
        }
        if (!found) {
            found = &node->src;
        }
        ++n;
        if (n > 1) {
            found = NULL;
            break;
        }
    }
    zcs_media_source_registry_unlock();
    return (n == 1) ? found : NULL;
}

/** HLS 相对分片误解析为 /live/{prefix}/{N}.ts 时，匹配唯一子流 prefix/<id> proxied/... */
/* 场景：m3u8 里的相对分片被一个粗糙的播放器直接当流名请求，形成 "/live/{prefix}/{N}"
 * 这种形态。这里按“前缀段 + 唯一子流”回查，同样要求唯一（n==1），歧义即放弃。 */
static zcs_media_source *find_single_live_stream_prefix(const char *vhost, const char *app,
                                                        const char *prefix)
{
    size_t plen;
    zcs_media_source *found = NULL;
    int n = 0;

    if (!vhost || !app || strcmp(app, "live") != 0 || !prefix || !prefix[0]) {
        return NULL;
    }
    plen = strlen(prefix);
    zcs_media_source_registry_lock();
    for (zcs_source_node *node = g_head; node; node = node->next) {
        const zcs_media_source *s = &node->src;
        if (!s->gop_queue || strcmp(s->vhost, vhost) != 0 || strcmp(s->app, app) != 0) {
            continue;
        }
        if (strncmp(s->stream, prefix, plen) != 0) {
            continue;
        }
        if (s->stream[plen] != '\0' && s->stream[plen] != '/') {
            continue;
        }
        if (s->stream[plen] == '\0') {
            continue;
        }
        if (!found) {
            found = &node->src;
        }
        ++n;
        if (n > 1) {
            found = NULL;
            break;
        }
    }
    zcs_media_source_registry_unlock();
    return (n == 1) ? found : NULL;
}

/* 按 stream 的“尾段 id”匹配（段边界须是 '/' 或整串）。同样要求唯一命中才返回，
 * 多路共享同一 id 后缀时返回 NULL——猜错会把观众带到另一路流。 */
static zcs_media_source *find_by_stream_id_suffix(const char *vhost, const char *app,
                                                  const char *id)
{
    if (!vhost || !app || !id || !id[0] || strchr(id, '/')) {
        return NULL;
    }
    zcs_media_source_registry_lock();
    zcs_media_source *found = NULL;
    int matches = 0;
    for (zcs_source_node *node = g_head; node; node = node->next) {
        const zcs_media_source *s = &node->src;
        if (!s->gop_queue || strcmp(s->vhost, vhost) != 0 || strcmp(s->app, app) != 0) {
            continue;
        }
        if (!stream_ends_with_id(s->stream, id)) {
            continue;
        }
        found = &node->src;
        ++matches;
        if (matches > 1) {
            found = NULL;
            break;
        }
    }
    zcs_media_source_registry_unlock();
    return found;
}

/*
 * 播放查找的核心兼容层：先按请求 schema 精确匹配，再按“协议族”回退。
 *
 * 为什么需要回退：同一个流可能通过不同协议接入（推流端用 RTMP，播端却用
 * RTSP/WebRTC 拉），schema 不完全对齐。约定：
 *   - RTSP/SRT 请求 → 依次兜底 RTP_PS、RTMP（这三种是常见的“同一路实时流”入口）；
 *   - RTMP 请求 → 依次兜底 SRT、RTSP、RTP_PS；
 *   - 无 schema（空）按 RTMP → RTP_PS 兜底。
 * 每一步都先按 (stream) 精确匹配，再按 stream_requested（推流原始名）匹配，
 * 因为对外 stream 名可能与推流时上报的不一致（见 find_by_stream_requested）。
 * 只要任一命中立即返回，保证“能用最贴近请求的那个源”。
 */
zcs_media_source *zcs_media_source_find_api_ex(const char *schema, const char *vhost,
                                               const char *app, const char *stream)
{
    char vh[ZCS_VHOST_MAX];
    zcs_media_source *s;

    if (!app || !stream) {
        return NULL;
    }
    zcs_vhost_normalize(vhost, vh, sizeof(vh));
    if (schema && schema[0]) {
        s = zcs_media_source_find_ex(schema, vh, app, stream);
        if (s) {
            return s;
        }
        s = find_by_stream_requested(schema, vh, app, stream);
        if (s) {
            return s;
        }
        if (strcmp(schema, ZCS_SCHEMA_RTSP) == 0 || strcmp(schema, ZCS_SCHEMA_SRT) == 0) {
            s = zcs_media_source_find_ex(ZCS_SCHEMA_RTP_PS, vh, app, stream);
            if (s) {
                return s;
            }
            s = find_by_stream_requested(ZCS_SCHEMA_RTP_PS, vh, app, stream);
            if (s) {
                return s;
            }
            s = zcs_media_source_find_ex(ZCS_SCHEMA_RTMP, vh, app, stream);
            if (s) {
                return s;
            }
            return find_by_stream_requested(ZCS_SCHEMA_RTMP, vh, app, stream);
        }
        if (strcmp(schema, ZCS_SCHEMA_RTMP) == 0) {
            s = zcs_media_source_find_ex(ZCS_SCHEMA_SRT, vh, app, stream);
            if (s) {
                return s;
            }
            s = find_by_stream_requested(ZCS_SCHEMA_SRT, vh, app, stream);
            if (s) {
                return s;
            }
            s = zcs_media_source_find_ex(ZCS_SCHEMA_RTSP, vh, app, stream);
            if (s) {
                return s;
            }
            s = find_by_stream_requested(ZCS_SCHEMA_RTSP, vh, app, stream);
            if (s) {
                return s;
            }
            s = zcs_media_source_find_ex(ZCS_SCHEMA_RTP_PS, vh, app, stream);
            if (s) {
                return s;
            }
            return find_by_stream_requested(ZCS_SCHEMA_RTP_PS, vh, app, stream);
        }
    }
    s = zcs_media_source_find_ex(ZCS_SCHEMA_RTMP, vh, app, stream);
    if (s) {
        return s;
    }
    s = find_by_stream_requested(ZCS_SCHEMA_RTMP, vh, app, stream);
    if (s) {
        return s;
    }
    s = zcs_media_source_find_ex(ZCS_SCHEMA_RTP_PS, vh, app, stream);
    if (s) {
        return s;
    }
    return find_by_stream_requested(ZCS_SCHEMA_RTP_PS, vh, app, stream);
}

zcs_media_source *zcs_media_source_find_api(const char *schema, const char *app, const char *stream)
{
    return zcs_media_source_find_api_ex(schema, ZCS_DEFAULT_VHOST, app, stream);
}

/* filter_match 里 schema 的特殊判断：请求 schema 为 RTSP/SRT 时视为“任意”放行。
 * 因为这两类是拉流协议，实际源可能以 RTP_PS/RTMP 形式存在，若严格按 schema 过滤
 * 就会把本该回忆源过滤掉。其余字段（vhost/app/stream）非空才参与比较。 */
static int filter_match(const zcs_media_source *s, const zcs_media_source_filter *f)
{
    if (!source_active(s)) {
        return 0;
    }
    if (!f) {
        return 1;
    }
    if (f->vhost && f->vhost[0] && strcmp(f->vhost, s->vhost) != 0) {
        return 0;
    }
    if (f->schema && f->schema[0]) {
        if (strcmp(f->schema, s->schema) != 0 && strcmp(f->schema, ZCS_SCHEMA_RTSP) != 0 &&
            strcmp(f->schema, ZCS_SCHEMA_SRT) != 0) {
            return 0;
        }
    }
    if (f->app && f->app[0] && strcmp(f->app, s->app) != 0) {
        return 0;
    }
    if (f->stream && f->stream[0] && strcmp(f->stream, s->stream) != 0) {
        return 0;
    }
    return 1;
}

void zcs_media_source_foreach(zcs_media_source_visit_fn fn, void *user,
                              const zcs_media_source_filter *filter)
{
    if (!fn) {
        return;
    }
    zcs_media_source_registry_lock();
    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (!filter_match(&node->src, filter)) {
            continue;
        }
        if (fn(&node->src, user) != 0) {
            break;
        }
    }
    zcs_media_source_registry_unlock();
}

void zcs_media_source_set_publisher(zcs_media_source *s, void *ctx,
                                    void (*kick)(void *ctx, int force))
{
    if (!s) {
        return;
    }
    zcs_media_source_registry_lock();
    if (s->publisher_kick && s->publisher_ctx && s->publisher_ctx != ctx) {
        s->publisher_kick(s->publisher_ctx, 1);
    }
    s->publisher_ctx = ctx;
    s->publisher_kick = kick;
    zcs_media_source_registry_unlock();
}

void zcs_media_source_clear_publisher(zcs_media_source *s, void *ctx)
{
    if (!s) {
        return;
    }
    zcs_media_source_registry_lock();
    if (s->publisher_ctx != ctx) {
        zcs_media_source_registry_unlock();
        return;
    }
    s->publisher_ctx = NULL;
    s->publisher_kick = NULL;
    zcs_media_source_registry_unlock();
}

/*
 * 关闭一路直播源：先取消 linger，再在锁内把 publisher 回调“摘下来”，
 * 关键点是 **kick 与 publish_fini 都在锁外调用**——kick 会回调推流/会话线程，
 * publish_fini 会派发事件，持锁调用它们有死锁与长时间持锁风险。
 * 摘回调是在锁内完成的（防止并发 unregister 重复 kick），拿到本地副本后
 * 释放锁再执行副作用。配置清理（clear）放在最后，避免清理期间读者拿到半清状态。
 */
int zcs_media_source_close(zcs_media_source *s, int force)
{
    if (!s) {
        return 0;
    }
    if (zcs_media_source_is_vod(s)) {
        zcs_vod_source_stop(s);
        return 1;
    }
    if (!s->gop_queue) {
        return 0;
    }
    zcs_media_linger_cancel(s->app, s->stream);
    zcs_media_source_registry_lock();
    int origin = s->publish_origin;
    int was_pub = s->publishing;
    void *pub = s->publisher_ctx;
    void (*kick)(void *ctx, int force) = s->publisher_kick;
    s->publishing = 0;
    s->publisher_ctx = NULL;
    s->publisher_kick = NULL;
    zcs_media_source_registry_unlock();

    if (kick) {
        kick(pub, force);
    }
    (void)pub;
    if (was_pub) {
        zcs_media_event_publish_fini(s, (zcs_media_origin)origin);
    }

    zcs_media_source_clear(s);
    return 1;
}

/*
 * 注册一个通用源（直播簇，带 gop_queue）。同名源已存在时按“重注册”处理：
 * 若旧节点是 VOD，先停掉再递归重建（VOD/直播不能共存于同一 key）；
 * 否则踢掉旧 publisher、清空其队列但**复用节点本身**，这样旧读者持有的
 * source 指针仍有效（避免整网读者悬空）。kick/fini 同样在锁外执行。
 */
zcs_media_source *zcs_media_source_register_ex(const char *schema, const char *vhost,
                                               const char *app, const char *stream)
{
    char vh[ZCS_VHOST_MAX];

    if (!schema || !app || !stream) {
        return NULL;
    }
    zcs_vhost_normalize(vhost, vh, sizeof(vh));

    zcs_media_source_registry_lock();

    zcs_media_source *exist = NULL;
    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, vh, app, stream)) {
            exist = &node->src;
            break;
        }
    }
    if (exist) {
        if (exist->vod_buffer) {
            zcs_media_source_registry_unlock();
            zcs_vod_source_stop(exist);
            return zcs_media_source_register_ex(schema, vh, app, stream);
        }
        zn_info("[source] publish_reregister app=%s stream=%s action=kick_prev", app, stream);
        int origin = exist->publish_origin;
        int was_pub = exist->publishing;
        void *pub = exist->publisher_ctx;
        void (*kick)(void *ctx, int force) = exist->publisher_kick;
        exist->publishing = 0;
        exist->publisher_ctx = NULL;
        exist->publisher_kick = NULL;
        zcs_media_source_registry_unlock();
        if (kick) {
            kick(pub, 1);
        }
        if (was_pub) {
            zcs_media_event_publish_fini(exist, (zcs_media_origin)origin);
        }
        zcs_media_source_clear(exist);
        return exist;
    }

    zcs_media_source *slot = source_node_alloc();
    if (!slot) {
        zcs_media_source_registry_unlock();
        zn_error("media source register failed: out of memory for %s/%s", app, stream);
        return NULL;
    }
    strncpy(slot->schema, schema, sizeof(slot->schema) - 1);
    strncpy(slot->vhost, vh, sizeof(slot->vhost) - 1);
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream, sizeof(slot->stream) - 1);
    slot->gop_queue = zcs_gop_queue_create();
    if (!slot->gop_queue) {
        source_node_free(slot);
        zcs_media_source_registry_unlock();
        zn_error("media source register failed: out of memory for %s/%s", app, stream);
        return NULL;
    }
    slot->create_stamp_ms = zn_monotonic_ms();
    {
        int total = registry_count_active();
        zcs_media_source_registry_unlock();
        zn_info("[source] source_register schema=%s vhost=%s app=%s stream=%s total=%d", schema,
                 vh, app, stream, total);
        return slot;
    }
}


zcs_media_source *zcs_media_source_register(const char *schema, const char *app, const char *stream)
{
    return zcs_media_source_register_ex(schema, ZCS_DEFAULT_VHOST, app, stream);
}

zcs_media_source *zcs_media_source_register_vod_ex(const char *schema, const char *vhost,
                                                   const char *app, const char *stream)
{
    zcs_media_source *slot;
    char vh[ZCS_VHOST_MAX];

    if (!schema || !app || !stream) {
        return NULL;
    }
    zcs_vhost_normalize(vhost, vh, sizeof(vh));

    zcs_media_source_registry_lock();

    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, vh, app, stream)) {
            zcs_media_source_registry_unlock();
            return &node->src;
        }
    }

    slot = source_node_alloc();
    if (!slot) {
        zcs_media_source_registry_unlock();
        return NULL;
    }
    strncpy(slot->schema, schema, sizeof(slot->schema) - 1);
    strncpy(slot->vhost, vh, sizeof(slot->vhost) - 1);
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream, sizeof(slot->stream) - 1);
    slot->vod_buffer = zcs_vod_buffer_create(0);
    if (!slot->vod_buffer) {
        source_node_free(slot);
        zcs_media_source_registry_unlock();
        return NULL;
    }
    slot->create_stamp_ms = zn_monotonic_ms();
    slot->publish_origin = ZCS_ORIGIN_MP4_VOD;
    zcs_media_source_registry_unlock();
    zn_info("[source] vod_register vhost=%s app=%s stream=%s", vh, app, stream);
    return slot;
}


zcs_media_source *zcs_media_source_register_vod(const char *schema, const char *app,
                                                const char *stream)
{
    return zcs_media_source_register_vod_ex(schema, ZCS_DEFAULT_VHOST, app, stream);
}

void zcs_media_source_unregister_vod(zcs_media_source *s)
{
    if (!s || !s->vod_buffer) {
        return;
    }
    zcs_media_source_registry_lock();
    zn_info("[source] vod_unregister app=%s stream=%s", s->app, s->stream);
    zcs_vod_buffer_destroy(s->vod_buffer);
    source_node_free(s);
    zcs_media_source_registry_unlock();
}

/*
 * 推流注册：与 register_ex 的差别在于它额外记录 stream_requested（推流端原始名），
 * 供 find_by_stream_requested 用原始名回查。同名已在推流 → 先踢旧推流再复用节点；
 * 同名是 VOD → 拒绝（返回 NULL，不能覆盖点播）。复用节点时先释放锁做 kick/fini，
 * 再重新加锁统计 live 数，避免持锁调用会阻塞的外部逻辑。
 */
zcs_media_source *zcs_media_source_register_publish_ex(const char *schema, const char *vhost,
                                                       const char *app,
                                                       const char *stream_requested)
{
    zcs_media_source *exist;
    zcs_media_source *slot;
    int total;
    char vh[ZCS_VHOST_MAX];

    if (!schema || !app || !stream_requested || !stream_requested[0]) {
        return NULL;
    }
    zcs_vhost_normalize(vhost, vh, sizeof(vh));

    zcs_media_source_registry_lock();

    exist = NULL;
    for (zcs_source_node *node = g_head; node; node = node->next) {
        if (key_match(&node->src, schema, vh, app, stream_requested)) {
            exist = &node->src;
            break;
        }
    }
    if (exist) {
        if (exist->vod_buffer) {
            zcs_media_source_registry_unlock();
            return NULL;
        }
        zcs_media_linger_cancel(app, stream_requested);
        zn_info("[source] publish_reregister app=%s stream=%s", app, stream_requested);
        {
            int origin = exist->publish_origin;
            int was_pub = exist->publishing;
            void *pub = exist->publisher_ctx;
            void (*kick)(void *ctx, int force) = exist->publisher_kick;

            exist->publishing = 0;
            exist->publisher_ctx = NULL;
            exist->publisher_kick = NULL;
            strncpy(exist->stream_requested, stream_requested, sizeof(exist->stream_requested) - 1);
            exist->stream_requested[sizeof(exist->stream_requested) - 1] = '\0';
            zcs_media_source_registry_unlock();
            if (kick) {
                kick(pub, 1);
            }
            if (was_pub) {
                zcs_media_event_publish_fini(exist, (zcs_media_origin)origin);
            }
            zcs_media_source_clear(exist);
            zcs_media_source_registry_lock();
            total = registry_count_live();
            zcs_media_source_registry_unlock();
            zn_info("[source] publish_reuse app=%s stream=%s total=%d", app, stream_requested,
                     total);
            return exist;
        }
    }

    slot = source_node_alloc();
    if (!slot) {
        zcs_media_source_registry_unlock();
        zn_error("media source register_publish failed: out of memory app=%s stream=%s", app,
                  stream_requested);
        return NULL;
    }
    strncpy(slot->schema, schema, sizeof(slot->schema) - 1);
    strncpy(slot->vhost, vh, sizeof(slot->vhost) - 1);
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream_requested, sizeof(slot->stream) - 1);
    strncpy(slot->stream_requested, stream_requested, sizeof(slot->stream_requested) - 1);
    slot->gop_queue = zcs_gop_queue_create();
    if (!slot->gop_queue) {
        source_node_free(slot);
        zcs_media_source_registry_unlock();
        zn_error("media source register_publish failed: out of memory app=%s stream=%s", app,
                  stream_requested);
        return NULL;
    }

    slot->create_stamp_ms = zn_monotonic_ms();
    total = registry_count_live();
    zcs_media_source_registry_unlock();

    zn_info("[source] publish_start vhost=%s app=%s stream=%s total=%d", vh, app,
             stream_requested, total);
    return slot;
}


zcs_media_source *zcs_media_source_register_publish(const char *schema, const char *app,
                                                    const char *stream_requested)
{
    return zcs_media_source_register_publish_ex(schema, ZCS_DEFAULT_VHOST, app,
                                                stream_requested);
}

void zcs_media_source_clear(zcs_media_source *s)
{
    if (!s || !s->gop_queue) {
        return;
    }
    zn_info("[source] source_clear app=%s stream=%s video=%d audio=%d", s->app, s->stream,
             s->has_video, s->has_audio);
    zcs_gop_queue_clear(s->gop_queue);
    zcs_media_stats_reset(s);
    s->has_video = 0;
    s->has_audio = 0;
    zcs_video_track_clear(&s->video);
    zcs_audio_track_clear(&s->audio);
}

void zcs_media_source_unregister(zcs_media_source *s)
{
    if (!s || !s->gop_queue) {
        return;
    }
    zcs_media_source_registry_lock();
    zn_info("[source] source_unregister app=%s stream=%s", s->app, s->stream);
    zcs_gop_queue_destroy(s->gop_queue);
    source_node_free(s);
    zcs_media_source_registry_unlock();
}

const uint8_t *zcs_media_source_video_config(const zcs_media_source *s, size_t *len)
{
    if (!s) {
        return NULL;
    }
    zcs_media_source_registry_lock();
    const uint8_t *p = NULL;
    if (s->gop_queue) {
        p = zcs_gop_queue_video_config(s->gop_queue, len);
    } else if (s->vod_buffer) {
        p = zcs_vod_buffer_video_config(s->vod_buffer, len);
    }
    if (p && len && *len > 0) {
        zcs_media_source_registry_unlock();
        return p;
    }
    if (len) {
        *len = 0;
    }
    zcs_media_source_registry_unlock();
    return NULL;
}

const uint8_t *zcs_media_source_audio_config(const zcs_media_source *s, size_t *len)
{
    if (!s) {
        return NULL;
    }
    zcs_media_source_registry_lock();
    const uint8_t *p = NULL;
    if (s->gop_queue) {
        p = zcs_gop_queue_audio_config(s->gop_queue, len);
    } else if (s->vod_buffer) {
        p = zcs_vod_buffer_audio_config(s->vod_buffer, len);
    }
    if (p && len && *len > 0) {
        zcs_media_source_registry_unlock();
        return p;
    }
    if (len) {
        *len = 0;
    }
    zcs_media_source_registry_unlock();
    return NULL;
}

static zcs_codec_id play_video_codec(const zcs_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZCS_CODEC_INVALID;
    }
    if (s->video.ready && s->video.codec != ZCS_CODEC_INVALID) {
        return s->video.codec;
    }
    if (s->gop_queue) {
        cfg = zcs_gop_queue_video_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zcs_flv_tag_video_codec(cfg, len);
        }
    } else if (s->vod_buffer) {
        cfg = zcs_vod_buffer_video_config(s->vod_buffer, &len);
        if (cfg && len > 0) {
            return zcs_flv_tag_video_codec(cfg, len);
        }
    }
    return ZCS_CODEC_INVALID;
}

static zcs_codec_id play_audio_codec(const zcs_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZCS_CODEC_INVALID;
    }
    if (s->audio.ready && s->audio.codec != ZCS_CODEC_INVALID) {
        return s->audio.codec;
    }
    if (s->gop_queue) {
        cfg = zcs_gop_queue_audio_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zcs_flv_tag_audio_codec(cfg, len);
        }
    } else if (s->vod_buffer) {
        cfg = zcs_vod_buffer_audio_config(s->vod_buffer, &len);
        if (cfg && len > 0) {
            return zcs_flv_tag_audio_codec(cfg, len);
        }
    }
    return ZCS_CODEC_INVALID;
}

static int codec_gop_queue_playable(zcs_codec_id c, zcs_track_type track)
{
    if (track == ZCS_TRACK_VIDEO) {
        return c == ZCS_CODEC_H264 || c == ZCS_CODEC_H265 || c == ZCS_CODEC_AV1 ||
               c == ZCS_CODEC_VP8 || c == ZCS_CODEC_VP9 || c == ZCS_CODEC_H266;
    }
    if (track == ZCS_TRACK_AUDIO) {
        return c == ZCS_CODEC_AAC || c == ZCS_CODEC_G711A || c == ZCS_CODEC_G711U ||
               c == ZCS_CODEC_OPUS;
    }
    return 0;
}

int zcs_media_source_is_vod(const zcs_media_source *s)
{
    return s && s->vod_buffer && s->publish_origin == ZCS_ORIGIN_MP4_VOD;
}

/*
 * 判定是否走 gop_queue 播放路径（相对于黑盒转发）。只有编解码在 gop 侧可解封
 * 时才走：纯音频源只校验音频 codec；含视频源必须视频 codec 可播，且若带音频，
 * 音频 codec 也必须可播——否则 muxer 会在关键帧后方才失败的音频上出问题。
 */
int zcs_media_source_use_gop_queue_play(const zcs_media_source *s)
{
    zcs_codec_id vc, ac;

    if (!s || !s->gop_queue) {
        return 0;
    }
    if (!s->has_video) {
        ac = play_audio_codec(s);
        if (ac != ZCS_CODEC_INVALID) {
            return codec_gop_queue_playable(ac, ZCS_TRACK_AUDIO);
        }
        return 0;
    }
    vc = play_video_codec(s);
    if (!codec_gop_queue_playable(vc, ZCS_TRACK_VIDEO)) {
        return 0;
    }
    if (s->has_audio) {
        ac = play_audio_codec(s);
        if (ac != ZCS_CODEC_INVALID && !codec_gop_queue_playable(ac, ZCS_TRACK_AUDIO)) {
            return 0;
        }
    }
    return 1;
}

void zcs_media_subscriber_detach(zcs_media_subscriber *r)
{
    if (!r) {
        return;
    }
    if (r->gop) {
        zcs_gop_reader_detach(r->gop);
        r->gop = NULL;
    }
    if (r->vod) {
        zcs_vod_buffer_reader_detach(r->vod);
        r->vod = NULL;
    }
}

/*
 * 订阅一路直播源。先 detach 掉调用方可能残留的旧读者（复用 subscriber 句柄）。
 * seek_live=1 从头尾“最新处”接入并立即 seek_live 跳到直播边缘（新观众要低延迟）；
 * seek_live=0 从缓存开头接入（用于需要补关键帧/回看起点的场景）。
 */
zn_err_t zcs_media_source_subscribe(zcs_media_source *s, int seek_live, zcs_media_subscriber *out)
{
    if (!s || !out) {
        return ZN_ERR_INVALID;
    }
    zcs_media_subscriber_detach(out);
    if (!zcs_media_source_use_gop_queue_play(s) || !s->gop_queue) {
        return ZN_ERR_STATE;
    }
    if (seek_live) {
        out->gop = zcs_gop_reader_attach(s->gop_queue);
    } else {
        out->gop = zcs_gop_reader_attach_beginning(s->gop_queue);
    }
    if (!out->gop) {
        return ZN_ERR_NOMEM;
    }
    if (seek_live) {
        zcs_gop_reader_seek_live(out->gop);
    }
    return ZN_OK;
}

zn_err_t zcs_media_source_subscribe_gop(zcs_media_source *s, zcs_media_subscriber *out)
{
    if (!s || !out) {
        return ZN_ERR_INVALID;
    }
    zcs_media_subscriber_detach(out);
    if (!zcs_media_source_use_gop_queue_play(s) || !s->gop_queue) {
        return ZN_ERR_STATE;
    }
    out->gop = zcs_gop_reader_attach(s->gop_queue);
    if (!out->gop) {
        return ZN_ERR_NOMEM;
    }
    zcs_gop_reader_seek_gop_key(out->gop);
    return ZN_OK;
}

zn_err_t zcs_media_source_subscribe_vod(zcs_media_source *s, zcs_media_subscriber *out)
{
    if (!s || !out) {
        return ZN_ERR_INVALID;
    }
    zcs_media_subscriber_detach(out);
    if (!zcs_media_source_is_vod(s) || !s->vod_buffer) {
        return ZN_ERR_STATE;
    }
    out->vod = zcs_vod_buffer_reader_attach(s->vod_buffer, 1);
    if (!out->vod) {
        return ZN_ERR_NOMEM;
    }
    zcs_vod_buffer_reader_seek_video_key(out->vod);
    return ZN_OK;
}

/*
 * 面向播放请求的最终解析：在 find_api_ex 之上叠加一串“容错别名”尝试。
 * 顺序：精确/协议族 → 点播(必要时按需打开) → live 别名。live 分支专门兜住
 * 客户端五花八门的 URL 写法（VLC 只给 <id>、RTSP 老写法 stream/<id>、
 * 分片误当流名等）。每一步失败才继续，任一命中即返回，最大化“能播就播”。
 */
zcs_media_source *zcs_media_source_find_for_play_ex(const char *schema, const char *vhost,
                                                    const char *app, const char *stream)
{
    char vh[ZCS_VHOST_MAX];
    zcs_media_source *s;

    zcs_vhost_normalize(vhost, vh, sizeof(vh));
    s = zcs_media_source_find_api_ex(schema, vh, app, stream);
    if (s) {
        return s;
    }
    if (!app || !stream || !stream[0]) {
        return s;
    }
    s = zcs_vod_source_find_or_open_ex(schema, vh, app, stream, NULL);
    if (s) {
        return s;
    }
    if (strcmp(app, "live") != 0) {
        return NULL;
    }

    char alt[ZCS_STREAM_MAX];
    /* RTSP 推流曾用 stream/<id>，VLC 常只 play <id> */
    if (!strchr(stream, '/')) {
        int n = snprintf(alt, sizeof(alt), "stream/%s", stream);
        if (n > 0 && (size_t)n < sizeof(alt)) {
            s = zcs_media_source_find_api_ex(schema, vh, app, alt);
            if (s) {
                return s;
            }
        }
        s = find_by_stream_id_suffix(vh, app, stream);
        if (s) {
            return s;
        }
    } else {
        const char *tail = strrchr(stream, '/');
        if (tail && tail[1]) {
            s = find_by_stream_id_suffix(vh, app, tail + 1);
            if (s) {
                return s;
            }
        }
        if (strncmp(stream, "stream/", 7) == 0) {
            s = find_single_live_stream_alias(vh, app, "stream");
        }
        if (s) {
            return s;
        }
        s = find_single_live_stream_prefix(vh, app, stream);
        if (s) {
            return s;
        }
    }
    return find_single_live_stream_alias(vh, app, stream);
}

zcs_media_source *zcs_media_source_find_for_play(const char *schema, const char *app,
                                                 const char *stream)
{
    return zcs_media_source_find_for_play_ex(schema, ZCS_DEFAULT_VHOST, app, stream);
}
