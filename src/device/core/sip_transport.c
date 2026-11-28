/**
 * @file sip_transport.c
 * @brief SIP 传输接口：UDP server / TCP client + Content-Length 分帧。
 */
#include "device/core/sip_transport.h"
#include "zcs/util/dns_cache.h"

#include "zn/net/socket.h"
#include "zn/net/ssl.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tcp_server.h"
#include "zn/net/tls_client.h"
#include "zn/net/udp_server.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIP_MAX_MESSAGE (256u * 1024u)
#define SIP_RECONNECT_MS 3000u

typedef struct sip_conn {
    struct zcs_sip_transport *t;
    zn_tcp_session *session;
    char ip[64];
    uint16_t port;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
    struct sip_conn *next;
} sip_conn;

struct zcs_sip_transport {
    int mode;
    int server_mode;
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    zn_udp_server *udp;
    zn_tcp_client *tcp;
    zn_tls_client *tls;
    zn_ssl_ctx *ssl_ctx;
    zn_tcp_server *tcp_srv;
    zb_mutex *mtx;
    sip_conn *conns;
    int conn_count;
    zn_poller_timer *retry;

    char bind_host[64];
    uint16_t local_port;
    char remote_host[64];
    uint16_t remote_port;
    char tls_ca[512];
    char tls_cert[512];
    char tls_key[512];
    int tls_verify_peer;
    int tls_attempted;
    char peer_ip[64];
    uint16_t peer_port;

    int connected;
    zcs_sip_message_fn on_message;
    zcs_sip_state_fn on_state;
    void *user;

    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
};

static int ci_prefix(const uint8_t *p, size_t n, const char *s)
{
    size_t i;
    size_t len = strlen(s);

    if (n < len) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        char c = (char)p[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != s[i]) {
            return 0;
        }
    }
    return 1;
}

/*
 * 找到头部结束位置（返回头部长度，sep_len 给出分隔符长度）。
 * 同时兼容 CRLFCRLF(4) 与裸 LFLF(2) 两种换行——有些非标准设备省略 \r。
 * 先找 4 字节形式再找 2 字节形式，避免把 \r\n\r\n 错切成单个 \n\n 的尾部。
 */
static size_t header_end(const uint8_t *b, size_t n, size_t *sep_len)
{
    size_t i;

    for (i = 0; i + 4 <= n; ++i) {
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') {
            if (sep_len) {
                *sep_len = 4;
            }
            return i;
        }
    }
    for (i = 0; i + 2 <= n; ++i) {
        if (b[i] == '\n' && b[i + 1] == '\n') {
            if (sep_len) {
                *sep_len = 2;
            }
            return i;
        }
    }
    return (size_t)-1;
}

/*
 * 解析 Content-Length（含 SIP 压缩形式 "l:"）。头部大小写不敏感。
 *
 * 边界防御：长度被夹到 SIP_MAX_MESSAGE，防止对端报一个天文数字导致上层去等
 * 永远到不齐的 body（也可能被用来诱导巨额缓冲申请）。返回 0 表示无 body
 * （请求/部分响应正常如此）。
 */
static size_t header_content_length(const uint8_t *b, size_t hdr_len)
{
    size_t pos = 0;

    while (pos < hdr_len) {
        size_t eol = pos;
        size_t n;
        const uint8_t *line = b + pos;

        while (eol < hdr_len && b[eol] != '\n') {
            ++eol;
        }
        n = eol - pos;
        if ((ci_prefix(line, n, "content-length:") && n > 15) ||
            (ci_prefix(line, n, "l:") && n > 2)) {
            size_t off = ci_prefix(line, n, "l:") ? 2 : 15;
            unsigned long v = 0;
            while (off < n && (line[off] == ' ' || line[off] == '\t')) {
                ++off;
            }
            while (off < n && line[off] >= '0' && line[off] <= '9') {
                v = v * 10u + (unsigned long)(line[off] - '0');
                ++off;
            }
            if (v > SIP_MAX_MESSAGE) {
                return SIP_MAX_MESSAGE;
            }
            return (size_t)v;
        }
        pos = eol + 1;
    }
    return 0;
}

static void rx_reset(zcs_sip_transport *t)
{
    t->rx_len = 0;
}

/*
 * 追加接收字节到缓冲。两条防御：
 *   - 累计超过 SIP_MAX_MESSAGE 时清空重置：说明对端在发畸形/超长数据，继续
 *     增长只会吃内存，且下一次永远等不到完整消息；
 *   - 扩容按翻倍并封顶 SIP_MAX_MESSAGE，realloc 失败时静默丢弃本批（保留已存）。
 */
static void rx_append(zcs_sip_transport *t, const void *data, size_t len)
{
    if (t->rx_len + len > SIP_MAX_MESSAGE) {
        zn_warn("sip_transport: peer message exceeds %u bytes, reset", SIP_MAX_MESSAGE);
        rx_reset(t);
        return;
    }
    if (t->rx_len + len > t->rx_cap) {
        size_t cap = t->rx_cap ? t->rx_cap : 4096;
        uint8_t *p;
        while (cap < t->rx_len + len) {
            cap *= 2;
        }
        if (cap > SIP_MAX_MESSAGE) {
            cap = SIP_MAX_MESSAGE;
        }
        p = (uint8_t *)realloc(t->rx, cap);
        if (!p) {
            return;
        }
        t->rx = p;
        t->rx_cap = cap;
    }
    memcpy(t->rx + t->rx_len, data, len);
    t->rx_len += len;
}

/** 从流缓冲中切出完整 SIP 消息并回调；返回处理条数。 */
/* SIP over TCP 会粘包/半包，必须按“头部结束符 + Content-Length”自行分帧：
 * 头未完整、或 body 未到齐时保留在缓冲里等下次；切出一条后把剩余字节前移。
 * 一次可连续切多条（一个 TCP 段里常打包了多个 SIP 消息）。消息边界以
 * total = hdr + sep + clen 为准，绝不按 TCP 包边界切。 */
static int rx_drain(zcs_sip_transport *t)
{
    int n = 0;

    for (;;) {
        size_t sep = 0;
        size_t hdr = header_end(t->rx, t->rx_len, &sep);
        size_t clen;
        size_t total;

        if (hdr == (size_t)-1) {
            break;
        }
        clen = header_content_length(t->rx, hdr);
        total = hdr + sep + clen;
        if (t->rx_len < total) {
            break;
        }
        if (t->on_message) {
            t->on_message(t->user, t->peer_ip, t->peer_port, t->rx, total);
        }
        if (total < t->rx_len) {
            memmove(t->rx, t->rx + total, t->rx_len - total);
        }
        t->rx_len -= total;
        ++n;
    }
    return n;
}

static uint64_t retry_delay_cb(void *user);

/*
 * 断线重连：只在“未连接且没有已排程的重试”时才挂一个延迟定时器（3s）。
 * 双重去重是为了避免一次抖动触发多个重试定时器堆叠——每个 on_error 都会
 * 调到这里，但 t->retry 非空就跳过；重试回调执行前会把 t->retry 置空。
 */
static void schedule_retry(zcs_sip_transport *t)
{
    if (!t || t->retry || t->connected) {
        return;
    }
    t->retry = zn_poller_do_delay(t->poller, SIP_RECONNECT_MS, retry_delay_cb, t);
}

static void cancel_retry(zcs_sip_transport *t)
{
    if (t->retry) {
        zn_poller_timer_cancel(t->retry);
        t->retry = NULL;
    }
}

static int conn_rx_drain(sip_conn *c);

static void conn_rx_append(sip_conn *c, const void *data, size_t len)
{
    if (c->rx_len + len > SIP_MAX_MESSAGE) {
        c->rx_len = 0;
        return;
    }
    if (c->rx_len + len > c->rx_cap) {
        size_t cap = c->rx_cap ? c->rx_cap : 4096;
        uint8_t *p;
        while (cap < c->rx_len + len) {
            cap *= 2;
        }
        if (cap > SIP_MAX_MESSAGE) {
            cap = SIP_MAX_MESSAGE;
        }
        p = (uint8_t *)realloc(c->rx, cap);
        if (!p) {
            return;
        }
        c->rx = p;
        c->rx_cap = cap;
    }
    memcpy(c->rx + c->rx_len, data, len);
    c->rx_len += len;
}

static int conn_rx_drain(sip_conn *c)
{
    int n = 0;

    for (;;) {
        size_t sep = 0;
        size_t hdr = header_end(c->rx, c->rx_len, &sep);
        size_t clen;
        size_t total;

        if (hdr == (size_t)-1) {
            break;
        }
        clen = header_content_length(c->rx, hdr);
        total = hdr + sep + clen;
        if (c->rx_len < total) {
            break;
        }
        if (c->t->on_message) {
            c->t->on_message(c->t->user, c->ip, c->port, c->rx, total);
        }
        if (total < c->rx_len) {
            memmove(c->rx, c->rx + total, c->rx_len - total);
        }
        c->rx_len -= total;
        ++n;
    }
    return n;
}

static sip_conn *conn_find_locked(zcs_sip_transport *t, const char *ip, uint16_t port)
{
    sip_conn *c;

    for (c = t->conns; c; c = c->next) {
        if (strcmp(c->ip, ip) == 0 && c->port == port) {
            return c;
        }
    }
    return NULL;
}

static sip_conn *conn_primary_locked(zcs_sip_transport *t)
{
    return t->conns;
}

static void conn_unlink_locked(zcs_sip_transport *t, sip_conn *c)
{
    sip_conn **pp;

    for (pp = &t->conns; *pp; pp = &(*pp)->next) {
        if (*pp == c) {
            *pp = c->next;
            --t->conn_count;
            break;
        }
    }
}

/* ── UDP ──/* ── UDP ────────────────────────────────────────────────────────────────── */

static void udp_on_packet(zn_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                          const void *data, size_t len, void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    (void)srv;
    if (t->on_message) {
        t->on_message(t->user, peer_ip, peer_port, data, len);
    }
}

/* ── TCP ────────────────────────────────────────────────────────────────── */

/* 从已连接的 TCP socket 上取对端地址（用于给上层回包时填目标）和本地端口
 * （用于 SIP Via/Contact 公告）。二者任一取不到都不致命，只是相应字段留空。 */
static void tcp_update_peer(zcs_sip_transport *t)
{
    zn_socket *sock = zn_tcp_client_socket(t->tcp);
    char ip[64];
    uint16_t port = 0;

    if (!sock) {
        return;
    }
    if (zn_socket_get_peer(sock, ip, sizeof(ip), &port) == ZN_OK && ip[0]) {
        snprintf(t->peer_ip, sizeof(t->peer_ip), "%s", ip);
        t->peer_port = port;
    }
    if (zn_socket_get_local(sock, ip, sizeof(ip), &port) == ZN_OK && port) {
        t->local_port = port;
    }
}

static void tcp_on_connect(zn_tcp_client *client, void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    (void)client;
    t->connected = 1;
    cancel_retry(t);
    rx_reset(t);
    tcp_update_peer(t);
    zn_info("sip_transport: TCP connected %s:%u local=%u", t->remote_host, t->remote_port,
             t->local_port);
    if (t->on_state) {
        t->on_state(t->user, 1);
    }
}

static void tcp_on_recv(zn_tcp_client *client, const void *data, size_t len, void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    (void)client;
    rx_append(t, data, len);
    (void)rx_drain(t);
}

static void tcp_on_error(zn_tcp_client *client, void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    (void)client;
    if (t->connected) {
        zn_warn("sip_transport: TCP disconnected %s:%u", t->remote_host, t->remote_port);
    }
    t->connected = 0;
    rx_reset(t);
    if (t->on_state) {
        t->on_state(t->user, 0);
    }
    schedule_retry(t);
}

/* 客户端主动连接前先 close 掉旧连接：上一次失败的 connect 可能留下半开 socket
 * （poller 里仍有残留注册），不关会泄漏句柄并可能在旧 socket 上收到过期事件。
 * connect 返回非 OK 时不一定真失败（可能仍在异步 connecting），统一排程重试，
 * 由 on_connect/on_error 决定最终状态。 */
static void tcp_try_connect(zcs_sip_transport *t)
{
    char ipbuf[64];

    if (t->connected) {
        return;
    }
    zn_tcp_client_close(t->tcp);
    if (zn_tcp_client_connect(t->tcp, zcs_dns_target(t->remote_host, ipbuf, sizeof(ipbuf)),
                              t->remote_port) != ZN_OK) {
        zn_debug("sip_transport: connect %s:%u pending/failed", t->remote_host, t->remote_port);
        schedule_retry(t);
    }
}

/* ── TCP/TLS 服务端（平台连入） ─────────────────────────────────────────── */

/* 进程内同一时刻只允许一个 SIP 服务端（GB 设备端单实例）。 */
static zcs_sip_transport *g_srv_transport;

/*
 * 服务端每来一条 TCP/TLS 连接就建一个 sip_conn，挂到 t->conns 链表头。
 * 用独立链表（而非单 socket）是因为 GB28181 平台侧可能同时有多条信令连接；
 * 发送时按 peer 地址回查对应连接。链表操作全程持 t->mtx，因为可能在多个
 * poller 线程里被并发访问（连接建立/断开/发送分属不同回调线程）。
 */
static void *srv_session_create(zn_tcp_server *srv, zn_tcp_session *session)
{
    zcs_sip_transport *t = g_srv_transport;
    zn_debug("sip_transport: server session create");
    sip_conn *c = (sip_conn *)calloc(1, sizeof(*c));
    char ip[64];
    uint16_t port = 0;

    (void)srv;
    if (!c) {
        return NULL;
    }
    c->t = t;
    c->session = session;
    if (zn_socket_get_peer(zn_tcp_session_socket(session), ip, sizeof(ip), &port) == ZN_OK) {
        snprintf(c->ip, sizeof(c->ip), "%s", ip);
        c->port = port;
    }
    zb_mutex_lock(t->mtx);
    c->next = t->conns;
    t->conns = c;
    ++t->conn_count;
    if (!t->connected) {
        t->connected = 1;
    }
    zb_mutex_unlock(t->mtx);
    zn_info("sip_transport: %s peer connected %s:%u",
             t->mode == ZCS_SIP_TRANSPORT_TLS ? "TLS" : "TCP", c->ip, c->port);
    if (t->on_state) {
        t->on_state(t->user, 1);
    }
    return c;
}

static void srv_session_recv(zn_tcp_session *session, const void *data, size_t len, void *user)
{
    sip_conn *c = (sip_conn *)user;

    (void)session;
    if (!c) {
        return;
    }
    conn_rx_append(c, data, len);
    (void)conn_rx_drain(c);
}

/* 服务端连接断开：从链表摘除；只有摘掉最后一条连接时才置 connected=0 并上报
 * 离线——多条连接里断掉其中一条不影响整体在线状态。 */
static void srv_session_error(zn_tcp_session *session, void *user)
{
    sip_conn *c = (sip_conn *)user;
    zcs_sip_transport *t;
    int empty;

    if (!c) {
        return;
    }
    t = c->t;
    zb_mutex_lock(t->mtx);
    conn_unlink_locked(t, c);
    empty = t->conn_count == 0;
    if (empty) {
        t->connected = 0;
    }
    zb_mutex_unlock(t->mtx);
    zn_tcp_session_set_user(session, NULL);
    free(c->rx);
    free(c);
    if (empty && t->on_state) {
        t->on_state(t->user, 0);
    }
}

/*
 * 启动 SIP 服务端（TCP/TLS 平台连入场景）。进程内单实例：g_srv_transport 供
 * session_create 回调取回 transport（该回调没有 user 参数，只能靠全局）。
 * TLS 服务端必须配 cert/key；poller_pool 是必需的（多连接分散到各 poller）。
 * backlog=8 对 GB 信令足够，manager_interval_sec=-1 关闭空闲连接管理（GB 连接
 * 一般长连，不该被自动踢）。
 */
static int start_server(zcs_sip_transport *t)
{
    zn_tcp_server_opts_t sopts;
    zn_tcp_session_ops_t sops;

    if (!t->poller_pool) {
        zn_error("sip_transport: TCP/TLS server needs poller_pool");
        return -1;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TLS) {
        zn_ssl_config_t scfg;
        if (!t->tls_cert[0] || !t->tls_key[0]) {
            zn_error("sip_transport: TLS server needs tls_cert/tls_key");
            return -1;
        }
        zn_ssl_global_init();
        memset(&scfg, 0, sizeof(scfg));
        scfg.server_mode = 1;
        scfg.cert_file = t->tls_cert;
        scfg.key_file = t->tls_key;
        t->ssl_ctx = zn_ssl_ctx_create(&scfg);
        if (!t->ssl_ctx) {
            zn_error("sip_transport: TLS server context failed: %s", zn_last_error());
            return -1;
        }
    }
    memset(&sops, 0, sizeof(sops));
    sops.on_recv = srv_session_recv;
    sops.on_error = srv_session_error;
    memset(&sopts, 0, sizeof(sopts));
    sopts.host = t->bind_host;
    sopts.port = t->local_port;
    sopts.backlog = 8;
    sopts.poller_pool = t->poller_pool;
    sopts.session_ops = &sops;
    sopts.session_create_user = srv_session_create;
    sopts.manager_interval_sec = -1;
    sopts.ssl_ctx = t->ssl_ctx;
    g_srv_transport = t;
    t->tcp_srv = zn_tcp_server_create(&sopts);
    if (!t->tcp_srv || zn_tcp_server_start(t->tcp_srv) != ZN_OK) {
        zn_error("sip_transport: %s server listen %u failed",
                  t->mode == ZCS_SIP_TRANSPORT_TLS ? "TLS" : "TCP", t->local_port);
        return -1;
    }
    t->local_port = zn_tcp_server_port(t->tcp_srv);
    zn_info("sip_transport: %s server listening %s:%u",
             t->mode == ZCS_SIP_TRANSPORT_TLS ? "TLS" : "TCP", t->bind_host, t->local_port);
    return 0;
}

/* ── TLS ── *//* ── TLS ── */

static void tls_update_peer(zcs_sip_transport *t)
{
    zn_socket *sock = zn_tls_client_socket(t->tls);
    char ip[64];
    uint16_t port = 0;

    if (!sock) {
        return;
    }
    if (zn_socket_get_peer(sock, ip, sizeof(ip), &port) == ZN_OK && ip[0]) {
        snprintf(t->peer_ip, sizeof(t->peer_ip), "%s", ip);
        t->peer_port = port;
    }
    if (zn_socket_get_local(sock, ip, sizeof(ip), &port) == ZN_OK && port) {
        t->local_port = port;
    }
}

static void tls_on_connect(zn_tls_client *client, void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    (void)client;
    t->connected = 1;
    cancel_retry(t);
    rx_reset(t);
    tls_update_peer(t);
    zn_info("sip_transport: TLS connected %s:%u local=%u", t->remote_host, t->remote_port,
             t->local_port);
    if (t->on_state) {
        t->on_state(t->user, 1);
    }
}

static void tls_on_recv(zn_tls_client *client, const void *data, size_t len, void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    (void)client;
    rx_append(t, data, len);
    (void)rx_drain(t);
}

static void tls_on_error(zn_tls_client *client, void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    (void)client;
    if (t->connected) {
        zn_warn("sip_transport: TLS disconnected %s:%u", t->remote_host, t->remote_port);
    }
    t->connected = 0;
    rx_reset(t);
    if (t->on_state) {
        t->on_state(t->user, 0);
    }
    schedule_retry(t);
}

/* zn_tls_client stores the ops pointer; keep it static, not on stack. */
static const zn_tls_client_ops_t k_tls_ops = {
    tls_on_connect,
    tls_on_recv,
    tls_on_error,
};

static void tls_try_connect(zcs_sip_transport *t)
{
    char ipbuf2[64];
    zn_err_t err;

    if (t->connected) {
        return;
    }
    zn_info("sip_transport: TLS connect %s:%u ...", t->remote_host, t->remote_port);
    if (t->tls_attempted) {
        zn_tls_client_close(t->tls);
    }
    t->tls_attempted = 1;
    err = zn_tls_client_connect(t->tls, zcs_dns_target(t->remote_host, ipbuf2, sizeof(ipbuf2)),
                                t->remote_port);
    if (err != ZN_OK) {
        zn_debug("sip_transport: TLS connect failed err=%d (%s:%u)", (int)err, t->remote_host,
                  t->remote_port);
        schedule_retry(t);
    }
}

static uint64_t retry_delay_cb(void *user)
{
    zcs_sip_transport *t = (zcs_sip_transport *)user;

    t->retry = NULL;
    if (t->mode == ZCS_SIP_TRANSPORT_TLS) {
        tls_try_connect(t);
    } else {
        tcp_try_connect(t);
    }
    return 0;
}

/* ── 生命周期 ───────────────────────────────────────────────────────────── */

zcs_sip_transport *zcs_sip_transport_create(const zcs_sip_transport_opts *opts)
{
    zcs_sip_transport *t;

    if (!opts || !opts->poller) {
        return NULL;
    }
    t = (zcs_sip_transport *)calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }
    t->mode = opts->mode;
    t->poller = opts->poller;
    t->on_message = opts->on_message;
    t->on_state = opts->on_state;
    t->user = opts->user;
    t->local_port = opts->local_port;
    snprintf(t->bind_host, sizeof(t->bind_host), "%s",
             opts->bind_host && opts->bind_host[0] ? opts->bind_host : "0.0.0.0");
    if (opts->remote_host) {
        snprintf(t->remote_host, sizeof(t->remote_host), "%s", opts->remote_host);
    }
    t->remote_port = opts->remote_port;
    t->tls_verify_peer = opts->tls_verify_peer;
    t->server_mode = opts->server_mode;
    t->poller_pool = opts->poller_pool;
    if (opts->tls_ca) {
        snprintf(t->tls_ca, sizeof(t->tls_ca), "%s", opts->tls_ca);
    }
    if (opts->tls_cert) {
        snprintf(t->tls_cert, sizeof(t->tls_cert), "%s", opts->tls_cert);
    }
    if (opts->tls_key) {
        snprintf(t->tls_key, sizeof(t->tls_key), "%s", opts->tls_key);
    }
    t->mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    return t;
}

int zcs_sip_transport_start(zcs_sip_transport *t)
{
    if (!t) {
        return -1;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_UDP) {
        zn_udp_server_opts_t uopts;
        zn_udp_server_ops_t uops;

        memset(&uops, 0, sizeof(uops));
        uops.on_packet = udp_on_packet;
        memset(&uopts, 0, sizeof(uopts));
        uopts.host = t->bind_host;
        uopts.port = t->local_port;
        uopts.reuse = 1;
        uopts.poller = t->poller;
        uopts.ops = &uops;
        uopts.user = t;
        t->udp = zn_udp_server_create(&uopts);
        if (!t->udp || zn_udp_server_start(t->udp) != ZN_OK) {
            zn_error("sip_transport: UDP listen %u failed", t->local_port);
            return -1;
        }
        t->local_port = zn_udp_server_port(t->udp);
        t->connected = 1;
        if (t->on_state) {
            t->on_state(t->user, 1);
        }
        return 0;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TCP && t->server_mode) {
        return start_server(t);
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TLS && t->server_mode) {
        return start_server(t);
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TCP) {
        zn_tcp_client_opts_t copts;
        zn_tcp_client_ops_t cops;

        if (!t->remote_host[0] || !t->remote_port) {
            zn_error("sip_transport: TCP needs remote host/port");
            return -1;
        }
        memset(&cops, 0, sizeof(cops));
        cops.on_connect = tcp_on_connect;
        cops.on_recv = tcp_on_recv;
        cops.on_error = tcp_on_error;
        memset(&copts, 0, sizeof(copts));
        copts.poller = t->poller;
        copts.ops = &cops;
        copts.user = t;
        t->tcp = zn_tcp_client_create(&copts);
        if (!t->tcp) {
            return -1;
        }
        tcp_try_connect(t);
        return 0;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TLS) {
        zn_tls_client_opts_t copts;
        zn_ssl_config_t scfg;

        if (!t->remote_host[0] || !t->remote_port) {
            zn_error("sip_transport: TLS needs remote host/port");
            return -1;
        }
        zn_ssl_global_init();
        memset(&scfg, 0, sizeof(scfg));
        scfg.ca_file = t->tls_ca[0] ? t->tls_ca : NULL;
        scfg.verify_peer = t->tls_verify_peer ? 1 : 0;
        t->ssl_ctx = zn_ssl_ctx_create(&scfg);
        if (!t->ssl_ctx) {
            zn_error("sip_transport: TLS context create failed: %s", zn_last_error());
            return -1;
        }
        memset(&copts, 0, sizeof(copts));
        copts.poller = t->poller;
        copts.ssl_ctx = t->ssl_ctx;
        copts.ops = &k_tls_ops;
        copts.user = t;
        copts.sni_host = t->remote_host;
        t->tls = zn_tls_client_create(&copts);
        if (!t->tls) {
            zn_ssl_ctx_destroy(t->ssl_ctx);
            t->ssl_ctx = NULL;
            return -1;
        }
        tls_try_connect(t);
        return 0;
    }
    zn_error("sip_transport: unsupported mode %d", t->mode);
    return -1;
}

void zcs_sip_transport_stop(zcs_sip_transport *t)
{
    if (!t) {
        return;
    }
    cancel_retry(t);
    if (t->udp) {
        zn_udp_server_stop(t->udp);
        zn_udp_server_destroy(t->udp);
        t->udp = NULL;
    }
    if (t->tcp_srv) {
        zn_tcp_server_stop(t->tcp_srv);
        zn_tcp_server_destroy(t->tcp_srv);
        t->tcp_srv = NULL;
        if (g_srv_transport == t) {
            g_srv_transport = NULL;
        }
    }
    if (t->mtx) {
        zb_mutex_lock(t->mtx);
        while (t->conns) {
            sip_conn *c = t->conns;
            t->conns = c->next;
            free(c->rx);
            free(c);
        }
        t->conn_count = 0;
        zb_mutex_unlock(t->mtx);
    }
    if (t->tcp) {
        zn_tcp_client_close(t->tcp);
        zn_tcp_client_destroy(t->tcp);
        t->tcp = NULL;
    }
    if (t->tls) {
        zn_tls_client_close(t->tls);
        zn_tls_client_destroy(t->tls);
        t->tls = NULL;
    }
    if (t->ssl_ctx) {
        zn_ssl_ctx_destroy(t->ssl_ctx);
        t->ssl_ctx = NULL;
    }
    t->connected = 0;
}

void zcs_sip_transport_destroy(zcs_sip_transport *t)
{
    if (!t) {
        return;
    }
    zcs_sip_transport_stop(t);
    if (t->mtx) {
        zb_mutex_destroy(t->mtx);
        t->mtx = NULL;
    }
    free(t->rx);
    free(t);
}

/*
 * 本地端口对外语义：
 *   - 服务端模式：始终返回监听端口（连接建立与否都有意义）；
 *   - TCP/TLS 客户端模式：未连接时返回 0——此时还没有本地端口可公告；
 *   - UDP：始终返回绑定端口。
 * 调用方（GB 平台拼 Via/Contact）据此决定是否能把端口写进 SIP 头。
 */
uint16_t zcs_sip_transport_local_port(const zcs_sip_transport *t)
{
    if (!t) {
        return 0;
    }
    if (t->server_mode) {
        return t->local_port;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TCP || t->mode == ZCS_SIP_TRANSPORT_TLS) {
        return t->connected ? t->local_port : 0;
    }
    return t->local_port;
}

int zcs_sip_transport_is_connected(const zcs_sip_transport *t)
{
    return t && t->connected;
}

/*
 * 发送接口。四种模式路径：
 *   - UDP：按 peer 地址 sendto，无连接状态；
 *   - TCP/TLS 服务端：在连接表里按 peer 精确找，找不到且只有一条连接就用它；
 *   - TCP/TLS 客户端：走各自的 connected 通道。
 * 服务端模式拿锁查表后调用 send_async，出锁前完成，避免表项在发送期间被释放。
 */
int zcs_sip_transport_send(zcs_sip_transport *t, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len)
{
    if (!t || !data || len == 0) {
        return -1;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_UDP) {
        if (!t->udp || !peer_ip || !peer_ip[0] || !peer_port) {
            return -1;
        }
        return zn_udp_server_sendto(t->udp, peer_ip, peer_port, data, len) == ZN_OK ? 0 : -1;
    }
    if (t->server_mode && (t->mode == ZCS_SIP_TRANSPORT_TCP || t->mode == ZCS_SIP_TRANSPORT_TLS)) {
        sip_conn *c = NULL;
        int rc;

        if (!t->mtx) {
            return -1;
        }
        /* 定位目标连接：优先按 peer 地址精确匹配；只有一个连接时直接用它
         * （平台常只建一条连接，此时无需 peer 也能确定性送达）。 */
        zb_mutex_lock(t->mtx);
        if (peer_ip && peer_ip[0] && peer_port) {
            c = conn_find_locked(t, peer_ip, peer_port);
        }
        if (!c && t->conn_count == 1) {
            c = conn_primary_locked(t);
        }
        if (c) {
            rc = zn_tcp_session_send_async(c->session, data, len) == ZN_OK ? 0 : -1;
        } else {
            rc = -1;
        }
        zb_mutex_unlock(t->mtx);
        return rc;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TCP) {
        if (!t->tcp || !t->connected) {
            return -1;
        }
        return zn_tcp_client_send(t->tcp, data, len) == ZN_OK ? 0 : -1;
    }
    if (t->mode == ZCS_SIP_TRANSPORT_TLS) {
        if (!t->tls || !t->connected) {
            return -1;
        }
        return zn_tls_client_send(t->tls, data, len) == ZN_OK ? 0 : -1;
    }
    return -1;
}
