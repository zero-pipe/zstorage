/**
 * @file session.c
 * @brief WebRTC 会话生命周期与 IO 缓冲。
 *
 * 两类会话（mode）：PLAY（WHEP 拉流）与 PUBLISH（WHIP 推流）。二者共享同一套
 * ICE/UDP 绑定，只在数据分派与 teardown 上不同。
 *
 * 会话表是全局固定数组 + 互斥锁（g_webrtc_session_mtx）。锁内只做表操作与取
 * 字段快照；任何会阻塞/回调外部（destroy、event、ingest）的工作都先把手上的
 * 会话指针拷出来，释放锁后再做——避免持锁期间重入或长时间阻塞。
 *
 * 每个会话的 UDP socket 与 ICE agent 都绑定在其创建时的 poller 上，所有收发回调
 * 在该 poller 线程执行；会话本身的销毁由 reap/关闭流程触发，需注意仍有延迟回调
 * 指向它（用 peer_known、dtls_ready 等标志兜住）。
 */
#include "session/webrtc/session_internal.h"
#include "protocol/webrtc/ice_internal.h"
#include "protocol/webrtc/media_internal.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/media/media_limits.h"
#include "zcs/session/webrtc/webrtc_server.h"
#include "session/webrtc/play_session.h"
#include "session/webrtc/whip_ingress.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/util/buf_pool.h"
#include "zcs/core/event.h"
#include "zcs/play/dispatch.h"
#include "zb/os/sync.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_webrtc_session_no;
/* 会话表：按 [webrtc] max_sessions 在注册表首次初始化时一次性分配（默认 64，
 * 硬上限 ZCS_WEBRTC_SESSION_MAX_HARD）。用堆而非静态数组，是因为并发档位
 * 相差一个数量级时静态预留会白占内存；表本身指针数组，容量只影响指针大小。 */
static zcs_webrtc_session **g_webrtc_sessions;
static size_t g_webrtc_session_cap;
static int g_webrtc_session_count;
static zb_mutex *g_webrtc_session_mtx;
/** 配置的并发上限；zcs_webrtc_session_set_limit 在启动阶段、注册表使用前设置。 */
static size_t g_webrtc_session_limit = ZCS_WEBRTC_SESSION_MAX_DEFAULT;

void zcs_webrtc_session_set_limit(unsigned limit)
{
    if (limit == 0) {
        limit = ZCS_WEBRTC_SESSION_MAX_DEFAULT;
    }
    if (limit > ZCS_WEBRTC_SESSION_MAX_HARD) {
        zn_warn("[webrtc] max_sessions=%u exceeds hard cap %u; clamped", limit,
                 (unsigned)ZCS_WEBRTC_SESSION_MAX_HARD);
        limit = ZCS_WEBRTC_SESSION_MAX_HARD;
    }
    g_webrtc_session_limit = limit;
}

size_t zcs_webrtc_session_limit(void)
{
    return g_webrtc_session_limit;
}

static void webrtc_session_registry_init(void)
{
    if (!g_webrtc_session_mtx) {
        g_webrtc_session_mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    }
    if (!g_webrtc_sessions) {
        /* 容量取配置上限（不超过硬上限）：所有遍历都用 g_webrtc_session_cap 而不是
         * 编译期宏，避免"配置值 > 表容量"时越界。 */
        size_t cap = g_webrtc_session_limit;
        if (cap == 0 || cap > ZCS_WEBRTC_SESSION_MAX_HARD) {
            cap = ZCS_WEBRTC_SESSION_MAX_HARD;
        }
        g_webrtc_sessions = (zcs_webrtc_session **)calloc(cap, sizeof(*g_webrtc_sessions));
        if (g_webrtc_sessions) {
            g_webrtc_session_cap = cap;
        }
    }
}

static int webrtc_session_registry_full(void)
{
    return !g_webrtc_sessions || g_webrtc_session_count >= (int)g_webrtc_session_limit;
}

static int webrtc_session_reg_add(zcs_webrtc_session *s)
{
    int i;

    if (!s) {
        return -1;
    }
    webrtc_session_registry_init();
    zb_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        if (g_webrtc_sessions[i] == s) {
            zb_mutex_unlock(g_webrtc_session_mtx);
            return 0;
        }
    }
    if (g_webrtc_session_count >= (int)g_webrtc_session_limit) {
        zn_warn("[webrtc] session registry full (max=%u)", (unsigned)g_webrtc_session_limit);
        zb_mutex_unlock(g_webrtc_session_mtx);
        return -1;
    }
    g_webrtc_sessions[g_webrtc_session_count++] = s;
    zb_mutex_unlock(g_webrtc_session_mtx);
    return 0;
}

static void webrtc_session_reg_remove(zcs_webrtc_session *s)
{
    int i;

    if (!s || !g_webrtc_session_mtx) {
        return;
    }
    zb_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        if (g_webrtc_sessions[i] == s) {
            g_webrtc_sessions[i] = g_webrtc_sessions[g_webrtc_session_count - 1];
            g_webrtc_sessions[--g_webrtc_session_count] = NULL;
            break;
        }
    }
    zb_mutex_unlock(g_webrtc_session_mtx);
}

zcs_webrtc_session *zcs_webrtc_session_find(const char *id)
{
    int i;
    zcs_webrtc_session *s = NULL;

    if (!id || !id[0] || !g_webrtc_session_mtx) {
        return NULL;
    }
    zb_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        if (g_webrtc_sessions[i] && strcmp(g_webrtc_sessions[i]->id, id) == 0) {
            s = g_webrtc_sessions[i];
            break;
        }
    }
    zb_mutex_unlock(g_webrtc_session_mtx);
    return s;
}

int zcs_webrtc_session_io_buf_ensure(zcs_webrtc_session *s)
{
    if (!s || !s->poller) {
        return -1;
    }
    if (s->io_buf && s->io_cap >= ZCS_WEBRTC_PLAY_CRYPT_BYTES) {
        return 0;
    }
    if (!zcs_buf_pool_slot_resize_poller(&s->io_buf, &s->io_cap, ZCS_WEBRTC_PLAY_CRYPT_BYTES,
                                         s->poller)) {
        return -1;
    }
    return 0;
}

void zcs_webrtc_session_io_buf_release(zcs_webrtc_session *s)
{
    if (!s) {
        return;
    }
    zcs_buf_pool_slot_clear_poller(&s->io_buf, &s->io_cap, s->poller);
}

/* 收到非 STUN 数据：按会话模式分派（WHIP 入站 / DTLS 握手 / 媒体）。 */
/* 收到非 STUN 数据的分派：发布端交给 WHIP 入站；播放端在 DTLS 握手完成前走
 * “STUN/DTLS 握手”通道，之后才交给媒体解析（play_input）。这个分支是会话
 * 生命周期的关键闸门——dtls_ready 前后数据语义完全不同。 */
static void webrtc_ice_on_data(void *owner, const void *data, size_t len)
{
    zcs_webrtc_session *s = (zcs_webrtc_session *)owner;

    if (!s) {
        return;
    }
    if (s->mode == ZCS_WEBRTC_SESSION_PUBLISH) {
        zcs_webrtc_whip_ingress_on_udp(s, data, len);
        return;
    }
    if (!s->dtls_ready) {
        (void)zcs_webrtc_play_on_stun_dtls(s, data, len);
    } else {
        zcs_webrtc_play_input(s, data, len);
    }
}

/* ICE 连通：发布端与 client 模式主动发起 DTLS；Play 保留 agent 应答 consent。 */
/* 只有需要“我们主动握手”的角色（发布/主动 client）在连通后立即发起 DTLS；
 * 被动 Play 端等对端的 ClientHello 经 on_data 触发，避免双方同时当 client。 */
static void webrtc_ice_on_connected(void *owner)
{
    zcs_webrtc_session *s = (zcs_webrtc_session *)owner;

    if (!s) {
        return;
    }
    if (s->mode == ZCS_WEBRTC_SESSION_PUBLISH || s->ice_host.client_mode) {
        zcs_webrtc_session_try_dtls_client(s);
    }
}

/* ICE 模块与会话层的唯一绑定：填充 host 的发送口/标识/回调。 */
static void webrtc_ice_host_init(zcs_webrtc_session *s)
{
    s->ice_host.owner = s;
    s->ice_host.poller = s->poller;
    s->ice_host.udp = s->udp;
    s->ice_host.id = s->id;
    s->ice_host.publish = (s->mode == ZCS_WEBRTC_SESSION_PUBLISH);
    s->ice_host.on_data = webrtc_ice_on_data;
    s->ice_host.on_connected = webrtc_ice_on_connected;
}

static void webrtc_on_udp_packet(zn_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                                 const void *data, size_t len, void *user)
{
    zcs_webrtc_session *s = (zcs_webrtc_session *)user;
    (void)srv;
    if (s) {
        zcs_webrtc_session_on_udp(s, peer_ip, peer_port, data, len);
    }
}

static zn_err_t webrtc_session_bind_udp(zcs_webrtc_session *s)
{
    zcs_webrtc_server *svc = zcs_webrtc_server_instance();
    zn_udp_server_opts_t uopts;
    uint16_t port_min = 0;
    uint16_t port_max = 0;
    int have_range;
    static const zn_udp_server_ops_t ops = {webrtc_on_udp_packet};
    if (!s || !s->poller) {
        return ZN_ERR_INVALID;
    }
    memset(&uopts, 0, sizeof(uopts));
    uopts.host = zcs_webrtc_server_bind_host(svc);
    uopts.reuse = 1;
    uopts.poller = s->poller;
    uopts.ops = &ops;
    uopts.user = s;

    /* 优先在配置的端口范围内绑定：NAT/防火墙只放通固定区间时必需。
     * 范围内无可用端口则回退系统临时端口，避免会话直接失败。 */
    have_range = zcs_webrtc_server_port_range(svc, &port_min, &port_max);
    if (have_range) {
        unsigned span = (unsigned)(port_max - port_min) + 1u;
        unsigned start = span > 1u ? (unsigned)(rand() % span) : 0u;
        unsigned i;

        for (i = 0; i < span; ++i) {
            unsigned p = (unsigned)port_min + (start + i) % span;

            /* 范围内必须独占端口：reuse=1 会让"已被本进程别的会话占用"的端口
             * 也绑定成功，两个会话抢同一端口 → ICE 互相打不通。
             * 临时端口（port=0）才使用 reuse，此处不复用。 */
            uopts.reuse = 0;
            uopts.port = (uint16_t)p;
            s->udp = zn_udp_server_create(&uopts);
            if (s->udp && zn_udp_server_start(s->udp) == ZN_OK) {
                break;
            }
            if (s->udp) {
                zn_udp_server_destroy(s->udp);
                s->udp = NULL;
            }
        }
        if (!s->udp) {
            zn_warn("[webrtc] no free UDP port in [%u,%u], falling back to ephemeral",
                     (unsigned)port_min, (unsigned)port_max);
        }
    }
    if (!s->udp) {
        uopts.reuse = 1;
        uopts.port = 0;
        s->udp = zn_udp_server_create(&uopts);
        if (!s->udp || zn_udp_server_start(s->udp) != ZN_OK) {
            return ZN_ERR_STATE;
        }
    }
    s->port = zn_udp_server_port(s->udp);
    webrtc_ice_host_init(s);
    s->ice_host.ctx = zcs_webrtc_ice_create(&s->ice_host);
    if (!s->ice_host.ctx) {
        zn_udp_server_stop(s->udp);
        zn_udp_server_destroy(s->udp);
        s->udp = NULL;
        return ZN_ERR_STATE;
    }
    return ZN_OK;
}

static void webrtc_session_unbind_udp(zcs_webrtc_session *s)
{
    if (!s) {
        return;
    }
    if (s->ice_host.ctx) {
        zcs_webrtc_ice_destroy(s->ice_host.ctx);
        s->ice_host.ctx = NULL;
    }
    if (s->udp) {
        zn_udp_server_stop(s->udp);
        zn_udp_server_destroy(s->udp);
        s->udp = NULL;
        s->ice_host.udp = NULL;
    }
}

/* 报满时先尝试回收空闲会话，再判满——这样“到达上限”时不是立刻拒新会话，而是
 * 先给一次清理机会。锁分两段：先判满/解锁，再在锁内加表（add 自己会加锁）由调用方
 * 在 bind_udp 成功后完成。 */
zcs_webrtc_session *zcs_webrtc_session_create(zcs_media_source *src, const char *app,
                                              const char *stream, zn_poller *poller)
{
    zcs_webrtc_session *s;
    if (!src || !app || !stream || !poller) {
        return NULL;
    }
    webrtc_session_registry_init();
    zcs_webrtc_session_reap_idle(zn_monotonic_ms());
    zb_mutex_lock(g_webrtc_session_mtx);
    if (webrtc_session_registry_full()) {
        zn_warn("[webrtc] WHEP create rejected: session limit %u",
                 (unsigned)g_webrtc_session_limit);
        zb_mutex_unlock(g_webrtc_session_mtx);
        return NULL;
    }
    zb_mutex_unlock(g_webrtc_session_mtx);
    s = (zcs_webrtc_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->poller = poller;
    s->source = src;
    strncpy(s->app, app, sizeof(s->app) - 1);
    strncpy(s->stream, stream, sizeof(s->stream) - 1);
    ++g_webrtc_session_no;
    s->session_no = g_webrtc_session_no;
    s->mode = ZCS_WEBRTC_SESSION_PLAY;
    s->created_ms = s->last_rx_ms = s->last_tx_ms = zn_monotonic_ms();
    snprintf(s->id, sizeof(s->id), "whep%u", s->session_no);
    if (webrtc_session_bind_udp(s) != ZN_OK) {
        free(s);
        return NULL;
    }
    if (webrtc_session_reg_add(s) != 0) {
        webrtc_session_unbind_udp(s);
        free(s);
        return NULL;
    }
    zn_info("[webrtc] session #%u id=%s app=%s stream=%s port=%u", s->session_no, s->id, app,
             stream, (unsigned)s->port);
    return s;
}

/* 创建 WHIP 发布会话：先建 ingest（它负责解复用/发布到流注册表），source 取自
 * ingest。顺序很重要——bind_udp 成功后再入表，任何一步失败都回滚，避免半初始化
 * 会话残留在表中。 */
zcs_webrtc_session *zcs_webrtc_session_create_publish(const char *vhost, const char *app,
                                                       const char *stream,
                                                      zn_poller *poller)
{
    zcs_webrtc_session *s;
    if (!app || !stream || !poller) {
        return NULL;
    }
    webrtc_session_registry_init();
    zcs_webrtc_session_reap_idle(zn_monotonic_ms());
    zb_mutex_lock(g_webrtc_session_mtx);
    if (webrtc_session_registry_full()) {
        zn_warn("[webrtc] WHIP create rejected: session limit %u",
                 (unsigned)g_webrtc_session_limit);
        zb_mutex_unlock(g_webrtc_session_mtx);
        return NULL;
    }
    zb_mutex_unlock(g_webrtc_session_mtx);
    s = (zcs_webrtc_session *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->poller = poller;
    s->mode = ZCS_WEBRTC_SESSION_PUBLISH;
    s->created_ms = s->last_rx_ms = s->last_tx_ms = zn_monotonic_ms();
    strncpy(s->app, app, sizeof(s->app) - 1);
    strncpy(s->stream, stream, sizeof(s->stream) - 1);
    s->ingest = zcs_live_ingest_create_vhost(vhost, app, stream, NULL);
    if (!s->ingest) {
        free(s);
        return NULL;
    }
    zcs_live_ingest_set_poller(s->ingest, poller);
    s->source = zcs_live_ingest_source(s->ingest);
    ++g_webrtc_session_no;
    s->session_no = g_webrtc_session_no;
    snprintf(s->id, sizeof(s->id), "whip%u", s->session_no);
    if (webrtc_session_bind_udp(s) != ZN_OK) {
        zcs_live_ingest_destroy(s->ingest);
        free(s);
        return NULL;
    }
    if (webrtc_session_reg_add(s) != 0) {
        webrtc_session_unbind_udp(s);
        zcs_live_ingest_destroy(s->ingest);
        free(s);
        return NULL;
    }
    zn_info("[webrtc] WHIP session #%u id=%s app=%s stream=%s port=%u", s->session_no, s->id, app,
             stream, (unsigned)s->port);
    return s;
}

int zcs_webrtc_session_enable_client(zcs_webrtc_session *s)
{
    if (!s || !s->poller) {
        return -1;
    }
    s->ice_host.client_mode = 1;
    s->dtls_as_client = 1;
    if (s->ice_host.ctx) {
        zcs_webrtc_ice_destroy(s->ice_host.ctx);
        s->ice_host.ctx = NULL;
    }
    s->ice_host.ctx = zcs_webrtc_ice_create(&s->ice_host);
    if (!s->ice_host.ctx) {
        zn_warn("[webrtc] client ICE create failed id=%s", s->id);
        return -1;
    }
    zn_info("[webrtc] client mode enabled id=%s mode=%d", s->id, (int)s->mode);
    return 0;
}

int zcs_webrtc_session_restart_ice(zcs_webrtc_session *s)
{
    if (!s || !s->poller) {
        return -1;
    }
    if (s->ice_host.ctx) {
        zcs_webrtc_ice_destroy(s->ice_host.ctx);
        s->ice_host.ctx = NULL;
    }
    s->ice_host.gather_expected = 0;
    s->ice_host.gather_done = 0;
    s->ice_host.peer_known = 0;
    s->ice_host.peer_ip[0] = '\0';
    s->ice_host.peer_port = 0;
    s->rt.rtcp_video_sr_sent = 0;
    s->rt.rtcp_audio_sr_sent = 0;
    s->ice_host.ctx = zcs_webrtc_ice_create(&s->ice_host);
    if (!s->ice_host.ctx) {
        return -1;
    }
    zn_info("[webrtc] ICE restart prepared id=%s client=%d", s->id, s->ice_host.client_mode);
    return 0;
}

/* 播放会话的拆除：解绑 play、摘除读者、上报 stop 事件。发布会话直接返回
 * （其收尾在 destroy 里走 whip_ingress_stop）。幂等：reader_attached 保证只摘一次。 */
void zcs_webrtc_session_teardown(zcs_webrtc_session *s)
{
    zcs_media_source *src;

    if (!s) {
        return;
    }
    if (s->mode == ZCS_WEBRTC_SESSION_PUBLISH) {
        return;
    }
    zcs_session_detach_play(ZCS_SESSION_WEBRTC, &s->play);
    src = s->source;
    if (s->play_reader_attached && src) {
        zcs_media_source_reader_remove(src);
        zcs_media_event_stop(src, "webrtc", s->play_start_ms);
        s->play_reader_attached = 0;
        s->play_start_ms = 0;
    }
}

void zcs_webrtc_session_destroy(zcs_webrtc_session *s)
{
    if (!s) {
        return;
    }
    webrtc_session_reg_remove(s);
    if (s->mode == ZCS_WEBRTC_SESSION_PUBLISH) {
        zcs_webrtc_whip_ingress_stop(s);
    } else {
        zcs_webrtc_session_teardown(s);
        zcs_webrtc_play_stop(s);
        if (s->play.readers.gop && s->source) {
            zcs_egress_source_close(&s->play);
        }
    }
    webrtc_session_unbind_udp(s);
    zcs_webrtc_session_io_buf_release(s);
    free(s);
}

/* 销毁全部会话。必须先把指针拷到本地数组并清空全局表（持锁内），再在锁外逐个
 * destroy——destroy 会回调事件/ingest/udp，若持锁调用既可能死锁，也会在遍历中途
 * 让表被改动。拷贝+清空的写法保证遍历稳定且锁窗口最小。 */
void zcs_webrtc_session_destroy_all(void)
{
    zcs_webrtc_session **pending;
    int n = 0;
    int i;

    webrtc_session_registry_init();
    if (!g_webrtc_sessions) {
        return;
    }
    /* 待删列表按表的实际容量分配：会话数可配置（最大上千），别放栈上。 */
    pending = (zcs_webrtc_session **)calloc(g_webrtc_session_cap, sizeof(*pending));
    if (!pending) {
        return;
    }
    zb_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count; ++i) {
        pending[n++] = g_webrtc_sessions[i];
    }
    g_webrtc_session_count = 0;
    memset(g_webrtc_sessions, 0, g_webrtc_session_cap * sizeof(*g_webrtc_sessions));
    zb_mutex_unlock(g_webrtc_session_mtx);
    for (i = 0; i < n; ++i) {
        zcs_webrtc_session_destroy(pending[i]);
    }
    free(pending);
}

int zcs_webrtc_session_add_remote_candidates(zcs_webrtc_session *s, const char *sdp, size_t len)
{
    if (!s || !sdp || len == 0) {
        return -1;
    }
    if (!s->ice_host.ctx) {
        return -1;
    }
    return zcs_webrtc_ice_add_remote_sdp(s->ice_host.ctx, sdp, len);
}

size_t zcs_webrtc_session_stats_all(zcs_webrtc_session_stats *out, size_t cap)
{
    size_t n = 0;
    int i;

    if (!out || cap == 0) {
        return 0;
    }
    webrtc_session_registry_init();
    if (!g_webrtc_session_mtx) {
        return 0;
    }
    zb_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count && n < cap; ++i) {
        zcs_webrtc_session *s = g_webrtc_sessions[i];

        if (!s) {
            continue;
        }
        memset(&out[n], 0, sizeof(out[n]));
        snprintf(out[n].id, sizeof(out[n].id), "%s", s->id);
        snprintf(out[n].app, sizeof(out[n].app), "%s", s->app);
        snprintf(out[n].stream, sizeof(out[n].stream), "%s", s->stream);
        out[n].session_no = s->session_no;
        out[n].mode = (int)s->mode;
        out[n].rtpq_pkts = s->rt.q_n;
        out[n].rtpq_bytes = s->rt.q_bytes;
        out[n].rtpq_bytes_max = s->rt.q_bytes_max;
        out[n].rtpq_dropped_bytes = s->rt.q_dropped_bytes;
        out[n].rtpq_dropped_pkts = s->rt.q_dropped_pkts;
        out[n].rtpq_alloc_fail = s->rt.q_alloc_fail;
        out[n].nack_cache_pkts = s->rt.nack_n;
        out[n].nack_cache_bytes = s->rt.nack_bytes;
        out[n].nack_hits = s->rt.nack_hits;
        out[n].nack_misses = s->rt.nack_misses;
        out[n].nack_retransmits = s->rt.nack_retransmits;
        out[n].rtcp_rr_pkts = s->rt.rtcp_rr_pkts;
        out[n].rtcp_fraction_lost = s->rt.rtcp_fraction_lost;
        out[n].rtcp_jitter = s->rt.rtcp_jitter;
        out[n].rtcp_rtt_ms = s->rt.rtcp_rtt_ms;
        out[n].twcc_ext_id = s->rt.twcc_ext_id;
        out[n].twcc_fb_pkts = s->rt.twcc_fb_pkts;
        out[n].twcc_received = s->rt.twcc_received;
        out[n].twcc_lost = s->rt.twcc_lost;
        out[n].twcc_loss_permille = s->rt.twcc_loss_permille;
        out[n].twcc_loss_ewma = s->rt.twcc_loss_ewma;
        out[n].send_bps = s->rt.send_bps;
        out[n].pace_waits = s->rt.pace_waits;
        out[n].adapt_bps = s->rt.adapt_window_bps;
        out[n].adapt_dropping = s->rt.adapt_dropping;
        out[n].adapt_wait_ms = s->rt.adapt_wait_ewma_ms;
        out[n].adapt_resets = s->rt.adapt_resets;
        out[n].adapt_dropped_frames = s->rt.adapt_dropped_frames;
        out[n].adapt_dropped_bytes = s->rt.adapt_dropped_bytes;
        out[n].pli_count = s->rt.pli_count;
        out[n].pli_suppressed = s->rt.pli_suppressed;
        out[n].talk_enabled = s->talk_enabled;
        out[n].talk_rx_pkts = s->talk_rx_pkts;
        out[n].talk_rx_bytes = s->talk_rx_bytes;
        out[n].whip_rx_pkts = s->rt.whip_rx_pkts;
        out[n].whip_rx_bytes = s->rt.whip_rx_bytes;
        out[n].whip_rx_dropped = s->rt.whip_rx_dropped;
        out[n].whip_srtp_fail = s->rt.whip_srtp_fail;
        out[n].created_ms = s->created_ms;
        out[n].last_rx_ms = s->last_rx_ms;
        ++n;
    }
    zb_mutex_unlock(g_webrtc_session_mtx);
    return n;
}

/*
 * 回收空闲/久不连通的会话。两个超时语义不同：
 *   - CONNECT_TIMEOUT：创建后一直没完成 DTLS、也没起媒体 → 连接从未建立，按连接超时回收；
 *   - IDLE_TIMEOUT：已建立过，但最近无收包 → 对端失联。
 * client_mode 会话跳过：它们的生命周期归 client 模块管理，不在此回收。
 * 同样先持锁筛出待删列表，再解锁逐个 destroy。
 */
void zcs_webrtc_session_reap_idle(uint64_t now_ms)
{
    zcs_webrtc_session **pending;
    size_t cap;
    int n = 0;
    int i;

    webrtc_session_registry_init();
    if (!g_webrtc_session_mtx || !g_webrtc_sessions) {
        return;
    }
    cap = g_webrtc_session_cap;
    if (cap == 0) {
        return;
    }
    /* 锁外分配待删列表：表容量可配置（最大上千），不能放栈上；也不在锁内 calloc。 */
    pending = (zcs_webrtc_session **)calloc(cap, sizeof(*pending));
    if (!pending) {
        return;
    }
    zb_mutex_lock(g_webrtc_session_mtx);
    for (i = 0; i < g_webrtc_session_count && (size_t)n < cap; ++i) {
        zcs_webrtc_session *s = g_webrtc_sessions[i];
        uint64_t last;

        if (!s) {
            continue;
        }
        if (s->ice_host.client_mode) {
            /* 主动连接的生命周期由 client 模块管理。 */
            continue;
        }
        last = s->last_rx_ms ? s->last_rx_ms : s->created_ms;
        if ((!s->dtls_ready && !s->media_started &&
             now_ms > s->created_ms &&
             now_ms - s->created_ms > ZCS_WEBRTC_SESSION_CONNECT_TIMEOUT_MS) ||
            (now_ms > last && now_ms - last > ZCS_WEBRTC_SESSION_IDLE_TIMEOUT_MS)) {
            pending[n++] = s;
        }
    }
    zb_mutex_unlock(g_webrtc_session_mtx);
    for (i = 0; i < n; ++i) {
        zn_warn("[webrtc] reap idle session id=%s app=%s stream=%s", pending[i]->id,
                 pending[i]->app, pending[i]->stream);
        zcs_webrtc_session_destroy(pending[i]);
    }
    free(pending);
}

void zcs_webrtc_session_on_udp(zcs_webrtc_session *s, const char *peer_ip, uint16_t peer_port,
                               const void *data, size_t len)
{
    if (!s || !data || len == 0) {
        return;
    }
    s->last_rx_ms = zn_monotonic_ms();
    if (s->ice_host.ctx) {
        zcs_webrtc_ice_on_udp(s->ice_host.ctx, peer_ip, peer_port, data, len);
        return;
    }
    if (peer_ip && peer_ip[0]) {
        strncpy(s->ice_host.peer_ip, peer_ip, sizeof(s->ice_host.peer_ip) - 1);
        s->ice_host.peer_port = peer_port;
        s->ice_host.peer_known = 1;
    }
    if (s->mode == ZCS_WEBRTC_SESSION_PUBLISH) {
        zcs_webrtc_whip_ingress_on_udp(s, data, len);
        return;
    }
    if (!s->dtls_ready) {
        (void)zcs_webrtc_play_on_stun_dtls(s, data, len);
        return;
    }
    zcs_webrtc_play_input(s, data, len);
}

/* 主动 DTLS client 角色：仅在需要主动连（PUBLISH/client_mode）、尚未就绪且 peer 已
 * 知时发起。kick 产生首包 ClientHello 直接发出了；kick 返回 1 表示握手已完成，
 * 置 dtls_ready，此后数据走媒体路径。 */
void zcs_webrtc_session_try_dtls_client(zcs_webrtc_session *s)
{
#if defined(ZCS_HAVE_WEBRTC_DTLS) && ZCS_HAVE_WEBRTC_DTLS
    uint8_t out[ZCS_WEBRTC_PLAY_DTLS_IO];
    size_t out_len;
    int st;
    if (!s || !s->dtls_as_client || s->dtls_ready || !s->ice_host.peer_known) {
        return;
    }
    if (!s->dtls) {
        s->dtls = zcs_webrtc_dtls_create_client();
    }
    if (!s->dtls) {
        return;
    }
    st = zcs_webrtc_dtls_kick(s->dtls, out, sizeof(out), &out_len);
    if (st < 0) {
        zn_warn("[webrtc] DTLS client kick failed id=%s", s->id);
        return;
    }
    if (out_len > 0) {
        zn_info("[webrtc] DTLS ClientHello id=%s peer=%s:%u bytes=%zu", s->id,
                 s->ice_host.peer_ip, (unsigned)s->ice_host.peer_port, out_len);
        zcs_webrtc_session_send_udp(s, out, out_len);
    }
    if (st == 1 && !s->dtls_ready) {
        s->dtls_ready = 1;
    }
#else
    (void)s;
#endif
}

/*
 * 发送 UDP，三种路径优先级：
 *   1) relay 候选：媒体必须经 TURN（ice_send），直发 peer 不可达；
 *   2) peer 已知：直接 sendto（最快，媒体突发走这条路，绕过 zice）；
 *   3) 兜底：交给 ice agent 发送（可能仍在候选选择阶段）。
 * sendto 失败日志做节流（前 8 次），避免网络故障时刷屏。
 */
int zcs_webrtc_session_send_udp(zcs_webrtc_session *s, const void *data, size_t len)
{
    zn_err_t err;

    if (!s || !data || len == 0) {
        return -1;
    }
#if defined(ZCS_WEBRTC_USE_ZICE) && ZCS_WEBRTC_USE_ZICE
    /* relay 候选：媒体必须走 TURN，不能直接发 peer。 */
    if (s->ice_host.ctx && zcs_webrtc_ice_uses_relay(s->ice_host.ctx)) {
        return zcs_webrtc_ice_send(s->ice_host.ctx, data, len) == 0 ? 0 : -1;
    }
#endif
    /* 媒体突发：peer 已知后始终直接 sendto（跳过 zice send 路径）。 */
    if (s->udp && s->ice_host.peer_known) {
        err = zn_udp_server_sendto(s->udp, s->ice_host.peer_ip, s->ice_host.peer_port, data, len);
        if (err == ZN_OK) {
            s->last_tx_ms = zn_monotonic_ms();
            return 0;
        }
        {
            static unsigned fail_log;

            if (fail_log < 8) {
                ++fail_log;
                zn_warn("[webrtc] udp sendto fail id=%s peer=%s:%u len=%zu err=%d", s->id,
                         s->ice_host.peer_ip, (unsigned)s->ice_host.peer_port, len, (int)err);
            }
        }
        return -1;
    }
#if defined(ZCS_WEBRTC_USE_ZICE) && ZCS_WEBRTC_USE_ZICE
    if (s->ice_host.ctx) {
        return zcs_webrtc_ice_send(s->ice_host.ctx, data, len) == 0 ? 0 : -1;
    }
#endif
    return -1;
}
