/**
 * @file gb_device.c
 * @brief GB28181 设备端：生命周期、SIP 传输、REGISTER/Keepalive、MANSCDP 查询/控制。
 */
#include "device/gb28181/gb28181_internal.h"
#include "zcs/device/core/device_command.h"
#include "zcs/device/core/device_event.h"
#include "zcs/device/core/device_ptz.h"

#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include "zcs/util/md5.h"
#include "util/strtok_r.h"
#include "zn/platform.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif
typedef SOCKET gb_sock_t;
#define GB_SOCK_INVALID INVALID_SOCKET
#define gb_sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int gb_sock_t;
#define GB_SOCK_INVALID (-1)
#define gb_sock_close close
#endif

/* GB28181 设备端是进程级单例（一台设备对应一个平台接入），全部状态放在
 * g_ctx 里。只有少数跨线程共享的计数器（sn/cseq）用 g_ctx.mtx 保护，其余
 * 字段都在该设备自己的单线程 poller 上访问。 */
static struct gb28181_ctx g_ctx;

struct gb28181_ctx *gb_ctx(void)
{
    return &g_ctx;
}

/* ── 工具 ───────────────────────────────────────────────────────────────── */

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", src);
}

/*
 * 探测本机到平台的出口 IP。技巧：对一个 UDP “连接”（不会真的发包）执行
 * connect 到远端:9，再 getsockname 读取内核为本路由选定的源地址——这比
 * 枚举网卡更准确，能拿到真正可达平台的那块网卡地址。任何一步失败都回落到
 * 127.0.0.1，保证总有可用的本地 IP 写进 SDP/Via。
 */
static int detect_local_ip(const char *remote, char *out, size_t cap)
{
    struct sockaddr_in addr;
    gb_sock_t s;
    struct sockaddr_in local;
    socklen_t len = sizeof(local);

    if (!out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    if (!remote || (addr.sin_addr.s_addr = inet_addr(remote)) == INADDR_NONE) {
        copy_str(out, cap, "127.0.0.1");
        return 0;
    }
    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == GB_SOCK_INVALID) {
        copy_str(out, cap, "127.0.0.1");
        return -1;
    }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(s, (struct sockaddr *)&local, &len) != 0) {
        gb_sock_close(s);
        copy_str(out, cap, "127.0.0.1");
        return -1;
    }
    snprintf(out, cap, "%s", inet_ntoa(local.sin_addr));
    gb_sock_close(s);
    return out[0] ? 0 : -1;
}

static uint32_t next_sn_locked(void)
{
    return ++g_ctx.sn;
}

/* ── 通道映射 ───────────────────────────────────────────────────────────── */

static void trim_inplace(char *s)
{
    char *p = s;
    char *e;

    if (!s) {
        return;
    }
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) {
        *--e = '\0';
    }
}

/*
 * 解析通道配置串：分号/逗号分隔的多个 "<id>:<vhost>/<app>/<stream>" 或
 * "<id>:<app>/<stream>"（省略 vhost）。
 * 每条通道注册进 device core，能力固定为 直播/回放/录像查询。解析时对
 * 局部副本做 trim/切分，不修改调用方的入参字符串。
 */
int gb_channels_load(const char *spec)
{
    char *buf;
    char *save = NULL;
    char *tok;
    int loaded = 0;

    if (!spec || !spec[0]) {
        return -1;
    }
    zcs_device_core_init();
    buf = (char *)malloc(strlen(spec) + 1);
    if (!buf) {
        return -1;
    }
    strcpy(buf, spec);

    for (tok = zcs_strtok_r(buf, ";,", &save); tok; tok = zcs_strtok_r(NULL, ";,", &save)) {
        char *colon;
        char *rest;
        char *slash;
        zcs_device_channel_info ch;

        trim_inplace(tok);
        if (!tok[0] || loaded >= ZCS_GB28181_CHANNEL_MAX) {
            continue;
        }
        colon = strchr(tok, ':');
        if (!colon) {
            zn_warn("gb28181: bad channel spec '%s' (expect <id>:<vhost>/<app>/<stream>)", tok);
            continue;
        }
        *colon = '\0';
        rest = colon + 1;
        trim_inplace(tok);
        trim_inplace(rest);
        if (!tok[0] || !rest[0]) {
            continue;
        }
        memset(&ch, 0, sizeof(ch));
        copy_str(ch.id, sizeof(ch.id), tok);
        copy_str(ch.gb_id, sizeof(ch.gb_id), tok);
        copy_str(ch.name, sizeof(ch.name), tok);
        while (*rest == '/') {
            ++rest;
        }
        slash = strchr(rest, '/');
        if (!slash) {
            zn_warn("gb28181: bad channel stream '%s' (expect app/stream)", rest);
            continue;
        }
        *slash = '\0';
        {
            char *second = slash + 1;
            char *third = strchr(second, '/');
            if (third) {
                *third = '\0';
                copy_str(ch.vhost, sizeof(ch.vhost), rest);
                copy_str(ch.app, sizeof(ch.app), second);
                copy_str(ch.stream, sizeof(ch.stream), third + 1);
            } else {
                copy_str(ch.vhost, sizeof(ch.vhost), ZCS_DEFAULT_VHOST);
                copy_str(ch.app, sizeof(ch.app), rest);
                copy_str(ch.stream, sizeof(ch.stream), second);
            }
        }
        ch.flags = ZCS_DEV_CH_GB;
        ch.caps = ZCS_DEV_CAP_LIVE | ZCS_DEV_CAP_PLAYBACK | ZCS_DEV_CAP_RECORD_QUERY;
        if (zcs_device_core_register(&ch) != 0) {
            zn_warn("gb28181: register channel %s failed", ch.id);
            continue;
        }
        ++loaded;
    }
    free(buf);
    return loaded;
}

int gb_channel_get(const char *id, gb_channel *out)
{
    if (!id || !out) {
        return -1;
    }
    return zcs_device_core_get(id, out);
}

/* 通道对应的媒体源：GB28181 通道在内部映射为 rtsp schema 的 app/stream，
 * 拉流/录像都通过这条统一的源查找入口，屏蔽具体协议差异。 */
zcs_media_source *gb_channel_source(const gb_channel *ch)
{
    if (!ch) {
        return NULL;
    }
    return zcs_media_source_find_for_play_ex("rtsp", ch->vhost, ch->app, ch->stream);
}

int gb_channel_online(const gb_channel *ch)
{
    return ch ? ch->online : 0;
}

/* ── SIP 发送 ───────────────────────────────────────────────────────────── */

static const char *gb_transport_name(void)
{
    return g_ctx.sip_transport_mode == ZCS_SIP_TRANSPORT_TCP ? "TCP" : "UDP";
}

int gb_transport_send(const char *ip, uint16_t port, const char *data, size_t len)
{
    if (!g_ctx.sip || !data || len == 0) {
        return -1;
    }
    /* server 模式：传输层已有一条到平台的连接，回复直接沿该连接写回，
     * 忽略目标 ip/port（传 NULL/0 让传输层走已连通道）。 */
    if (g_ctx.sip_server_mode) {
        ip = NULL;
        port = 0;
    }
    if (zcs_sip_transport_send(g_ctx.sip, ip, port, data, len) != 0) {
        zn_debug("gb28181: SIP send failed (%s:%u)", ip ? ip : "-", port);
        return -1;
    }
    return 0;
}

/*
 * 回一条 SIP 响应。目标地址优先 m->peer_ip（对端口发的）、否则平台地址：
 * 平台主动下发请求时源地址可能与配置的 platform_ip 不同（NAT/多网卡），
 * 把响应回给实际来源更可靠。to_tag/contact 允许为 NULL，交由构包函数按
 * 默认处理。
 */
void gb_send_response(const gb_sip_msg *m, int status, const char *reason, const char *to_tag,
                      const char *contact_user, const char *content_type, const void *body,
                      size_t body_len)
{
    char *buf;
    int n;
    const char *ip;
    uint16_t port;

    if (!m) {
        return;
    }
    buf = gb_sip_tx_alloc();
    if (!buf) {
        return;
    }
    n = gb_sip_build_response(buf, GB_SIP_MSG_MAX, m, status, reason, g_ctx.local_ip,
                              g_ctx.local_port, to_tag, contact_user, content_type, body,
                              body_len);
    if (n <= 0) {
        free(buf);
        return;
    }
    ip = m->peer_ip[0] ? m->peer_ip : g_ctx.platform_ip;
    port = m->peer_port ? m->peer_port : (uint16_t)g_ctx.platform_port;
    gb_transport_send(ip, port, buf, (size_t)n);
    free(buf);
}

/*
 * 主动发出一条 MANSCDP 消息（MESSAGE + Application/MANSCDP+xml）。
 * 目标地址优先用调用方给的 ip/port；为空则回落到“最近一次对端”再退到平台
 * 配置地址——保活/告警常拿不到实时地址，这条回退链保证总有一个可达目标。
 * cseq 在 mtx 内自增，与 REGISTER 共用同一计数器，符合 SIP 事务要求。
 */
int gb_send_manscdp(const char *ip, uint16_t port, const char *body)
{
    char *buf;
    char uri[128];
    char call_id[96];
    char from_tag[64];
    gb_sip_req_spec spec;
    int n;

    if (!body || !g_ctx.running) {
        return -1;
    }
    gb_rand_hex(call_id, 24);
    gb_rand_hex(from_tag, 12);
    snprintf(uri, sizeof(uri), "sip:%s@%s", g_ctx.platform_id, g_ctx.platform_domain);

    if (!ip || !ip[0]) {
        ip = g_ctx.last_peer_ip[0] ? g_ctx.last_peer_ip : g_ctx.platform_ip;
    }
    if (!port) {
        port = g_ctx.last_peer_port ? g_ctx.last_peer_port : (uint16_t)g_ctx.platform_port;
    }

    memset(&spec, 0, sizeof(spec));
    spec.method = "MESSAGE";
    spec.uri = uri;
    spec.from_user = g_ctx.device_id;
    spec.from_host = g_ctx.platform_domain;
    spec.from_tag = from_tag;
    spec.to_user = g_ctx.platform_id;
    spec.to_host = g_ctx.platform_domain;
    spec.call_id = call_id;
    spec.local_ip = g_ctx.local_ip;
    spec.local_port = g_ctx.local_port;
    spec.transport = gb_transport_name();
    spec.contact_user = g_ctx.device_id;
    spec.content_type = "Application/MANSCDP+xml";
    spec.body = body;
    spec.body_len = strlen(body);
    if (g_ctx.mtx) {
        zb_mutex_lock(g_ctx.mtx);
    }
    spec.cseq = ++g_ctx.cseq;
    if (g_ctx.mtx) {
        zb_mutex_unlock(g_ctx.mtx);
    }

    buf = gb_sip_tx_alloc();
    if (!buf) {
        return -1;
    }
    n = gb_sip_build_request(buf, GB_SIP_MSG_MAX, &spec);
    if (n <= 0) {
        free(buf);
        return -1;
    }
    n = gb_transport_send(ip, port, buf, (size_t)n);
    free(buf);
    return n;
}

/* ── REGISTER / Keepalive ───────────────────────────────────────────────── */

/*
 * 发送 REGISTER。with_auth 语义：0=不带鉴权首发；1/2=携带摘要鉴权（1 时
 * with_auth 也作为 digest 的算法选择透传）；其余非 0 也按摘要处理。
 *
 * 发了之后把报文正文与长度存进 g_ctx.tx_buf/tx_len 并置 tx_pending，用于
 * 超时重传——UDP 下 REGISTER 可能丢包，重传必须原样再发（含相同 Call-ID/
 * CSeq/鉴权 nonce），重新构造会改变计数器语义。cseq 递增在 mtx 内完成，
 * 因为其它线程（如告警）也会改 cseq。
 */
static void send_register(int with_auth)
{
    char buf[4096];
    char uri[128];
    char auth[512];
    gb_sip_req_spec spec;
    int n;

    if (!g_ctx.running) {
        return;
    }
    snprintf(uri, sizeof(uri), "sip:%s@%s", g_ctx.platform_id, g_ctx.platform_domain);
    auth[0] = '\0';
    if (with_auth && g_ctx.nonce[0]) {
        gb_digest_challenge ch;
        memset(&ch, 0, sizeof(ch));
        copy_str(ch.realm, sizeof(ch.realm), g_ctx.realm[0] ? g_ctx.realm : g_ctx.platform_domain);
        copy_str(ch.nonce, sizeof(ch.nonce), g_ctx.nonce);
        if (gb_digest_build_authorization(g_ctx.username[0] ? g_ctx.username : g_ctx.device_id,
                                          g_ctx.password, "REGISTER", uri, &ch,
                                          with_auth == 1 ? 1 : with_auth, auth,
                                          sizeof(auth)) != 0) {
            auth[0] = '\0';
        }
    }

    memset(&spec, 0, sizeof(spec));
    spec.method = "REGISTER";
    spec.uri = uri;
    spec.from_user = g_ctx.device_id;
    spec.from_host = g_ctx.platform_domain;
    spec.from_tag = g_ctx.from_tag;
    spec.to_user = g_ctx.platform_id;
    spec.to_host = g_ctx.platform_domain;
    spec.call_id = g_ctx.call_id;
    spec.local_ip = g_ctx.local_ip;
    spec.local_port = g_ctx.local_port;
    spec.transport = gb_transport_name();
    spec.contact_user = g_ctx.device_id;
    spec.has_expires = 1;
    spec.expires = g_ctx.register_expires;
    if (auth[0]) {
        spec.authorization = auth;
    }
    {
        unsigned cseq;
        if (g_ctx.mtx) {
            zb_mutex_lock(g_ctx.mtx);
        }
        cseq = ++g_ctx.cseq;
        if (g_ctx.mtx) {
            zb_mutex_unlock(g_ctx.mtx);
        }
        spec.cseq = cseq;
    }

    n = gb_sip_build_request(buf, sizeof(buf), &spec);
    if (n <= 0 || (size_t)n >= sizeof(g_ctx.tx_buf)) {
        return;
    }
    memcpy(g_ctx.tx_buf, buf, (size_t)n);
    g_ctx.tx_len = (size_t)n;
    g_ctx.tx_pending = 1;
    g_ctx.tx_deadline_ms = zn_monotonic_ms() + 500;
    g_ctx.reg_retry = 0;
    g_ctx.reg_state = 1;
    gb_transport_send(g_ctx.platform_ip, (uint16_t)g_ctx.platform_port, g_ctx.tx_buf, g_ctx.tx_len);
    zn_info("gb28181: REGISTER sent to %s:%u auth=%d", g_ctx.platform_ip, g_ctx.platform_port,
             auth[0] ? 1 : 0);
}

/* 发起注册。每次“全新”注册都重新生成 Call-ID/From-tag，避免上一次失败事务
 * 的标识被平台当成重复请求（某些平台会据此丢弃）；清空 registered/auth 状态
 * 保证从干净状态开始挑战握手。 */
void gb_register_start(void)
{
    gb_rand_hex(g_ctx.call_id, 24);
    gb_rand_hex(g_ctx.from_tag, 12);
    g_ctx.reg_auth = 0;
    g_ctx.reg_retry = 0;
    g_ctx.registered = 0;
    send_register(0);
}

/* 注销：发 expires=0 的 REGISTER 告诉平台“我下线了”。同样带鉴权（若平台
 * 之前挑战过），否则会收到 401 而注销失败。best-effort，不等待响应。 */
void gb_register_stop(void)
{
    char buf[4096];
    char uri[128];
    char auth[512];
    gb_sip_req_spec spec;
    int n;

    if (!g_ctx.sip || !g_ctx.running) {
        return;
    }
    snprintf(uri, sizeof(uri), "sip:%s@%s", g_ctx.platform_id, g_ctx.platform_domain);
    auth[0] = '\0';
    if (g_ctx.nonce[0]) {
        gb_digest_challenge ch;
        memset(&ch, 0, sizeof(ch));
        copy_str(ch.realm, sizeof(ch.realm), g_ctx.realm[0] ? g_ctx.realm : g_ctx.platform_domain);
        copy_str(ch.nonce, sizeof(ch.nonce), g_ctx.nonce);
        gb_digest_build_authorization(g_ctx.username[0] ? g_ctx.username : g_ctx.device_id,
                                      g_ctx.password, "REGISTER", uri, &ch, 2, auth,
                                      sizeof(auth));
    }
    memset(&spec, 0, sizeof(spec));
    spec.method = "REGISTER";
    spec.uri = uri;
    spec.from_user = g_ctx.device_id;
    spec.from_host = g_ctx.platform_domain;
    spec.from_tag = g_ctx.from_tag;
    spec.to_user = g_ctx.platform_id;
    spec.to_host = g_ctx.platform_domain;
    spec.call_id = g_ctx.call_id;
    spec.local_ip = g_ctx.local_ip;
    spec.local_port = g_ctx.local_port;
    spec.transport = gb_transport_name();
    spec.contact_user = g_ctx.device_id;
    spec.has_expires = 1;
    spec.expires = 0;
    if (auth[0]) {
        spec.authorization = auth;
    }
    spec.cseq = ++g_ctx.cseq;
    n = gb_sip_build_request(buf, sizeof(buf), &spec);
    if (n > 0) {
        gb_transport_send(g_ctx.platform_ip, (uint16_t)g_ctx.platform_port, buf, (size_t)n);
        zn_info("gb28181: unregister sent");
    }
    g_ctx.tx_pending = 0;
    g_ctx.reg_state = 0;
    g_ctx.registered = 0;
}

/* 保活：生成递增 SN 的 MANSCDP Keepalive 报文发往平台。SN 在 mtx 内自增，
 * 与查询/控制的 SN 共用同一序列，保证平台侧按 SN 去重/排序时不乱序。 */
static void send_keepalive(void)
{
    char body[512];
    char sn[24];
    uint32_t v;

    if (g_ctx.mtx) {
        zb_mutex_lock(g_ctx.mtx);
    }
    v = next_sn_locked();
    if (g_ctx.mtx) {
        zb_mutex_unlock(g_ctx.mtx);
    }
    snprintf(sn, sizeof(sn), "%u", v);
    gb_manscdp_build_keepalive(body, sizeof(body), g_ctx.device_id, sn);
    gb_send_manscdp(g_ctx.platform_ip, (uint16_t)g_ctx.platform_port, body);
}

/*
 * REGISTER 响应处理。
 *   401/407：平台要求鉴权，解析 WWW/Proxy-Authenticate 的 challenge，缓存在
 *     g_ctx.realm/nonce，最多挑战 3 次（reg_auth<3）后重发带鉴权的 REGISTER；
 *     超出仍失败则进入 30s 后重试状态。
 *   200：注册成功，排定 keepalive 与下次（提前 60s 或半周期的）refresh 注册。
 *   其它：视为失败，30s 后重试。
 * 这里体现 GB28181 的“注册续期”语义：expires 前的 refresh 必须主动发起，
 * 否则平台会判定离线。
 */
static void handle_register_response(const gb_sip_msg *m)
{
    g_ctx.tx_pending = 0;
    if (m->status == 401 || m->status == 407) {
        const char *www = gb_sip_hdr(m, "WWW-Authenticate");
        gb_digest_challenge ch;

        if (!www) {
            www = gb_sip_hdr(m, "Proxy-Authenticate");
        }
        if (www && gb_digest_parse_challenge(www, &ch) == 0 && g_ctx.reg_auth < 3) {
            copy_str(g_ctx.realm, sizeof(g_ctx.realm), ch.realm);
            copy_str(g_ctx.nonce, sizeof(g_ctx.nonce), ch.nonce);
            ++g_ctx.reg_auth;
            zn_info("gb28181: challenged, sending authenticated REGISTER");
            send_register(g_ctx.reg_auth);
            return;
        }
        g_ctx.reg_state = 3;
        g_ctx.next_reg_ms = zn_monotonic_ms() + 30000;
        zn_warn("gb28181: REGISTER auth failed");
        return;
    }
    if (m->status == 200) {
        uint64_t now = zn_monotonic_ms();
        uint64_t refresh;

        if (!g_ctx.registered) {
            zn_info("gb28181: registered as %s to %s", g_ctx.device_id, g_ctx.platform_ip);
        }
        g_ctx.registered = 1;
        g_ctx.reg_state = 2;
        g_ctx.reg_auth = 0;
        g_ctx.next_keepalive_ms = now + (uint64_t)g_ctx.keepalive_interval * 1000u;
        refresh = (uint64_t)g_ctx.register_expires * 1000u;
        refresh = refresh > 60000u ? refresh - 60000u : refresh / 2u;
        g_ctx.next_reg_ms = now + refresh;
        return;
    }
    zn_warn("gb28181: REGISTER status %d", m->status);
    g_ctx.registered = 0;
    g_ctx.reg_state = 3;
    g_ctx.next_reg_ms = zn_monotonic_ms() + 30000;
}

/*
 * 注册/保活状态机（每 200ms 由 on_tick 驱动）。reg_state 取值：
 *   0=初始，发首次无鉴权 REGISTER；
 *   1=已发出等待响应；超时则指数退避重传（step=500ms<<reg_retry，最多 4 次），
 *     全失败后转入 3；
 *   2=已注册：按 keepalive_interval 发心跳，到 next_reg_ms 做续期注册；
 *   3=失败/待重试：到 next_reg_ms 清 nonce 重新走无鉴权 REGISTER。
 * 续期时若手里有 nonce 就带鉴权（2），否则无鉴权首探（0），由 401 挑战驱动。
 */
void gb_register_tick(uint64_t now_ms)
{
    if (!g_ctx.running) {
        return;
    }
    if (g_ctx.tx_pending && now_ms >= g_ctx.tx_deadline_ms) {
        if (g_ctx.reg_retry < 4) {
            uint64_t step = 500u << g_ctx.reg_retry;
            ++g_ctx.reg_retry;
            g_ctx.tx_deadline_ms = now_ms + step;
            gb_transport_send(g_ctx.platform_ip, (uint16_t)g_ctx.platform_port, g_ctx.tx_buf,
                              g_ctx.tx_len);
        } else {
            zn_warn("gb28181: REGISTER timeout, retry in 30s");
            g_ctx.tx_pending = 0;
            g_ctx.reg_state = 3;
            g_ctx.next_reg_ms = now_ms + 30000;
        }
    }
    if (g_ctx.reg_state == 2) {
        if (now_ms >= g_ctx.next_keepalive_ms) {
            send_keepalive();
            g_ctx.next_keepalive_ms = now_ms + (uint64_t)g_ctx.keepalive_interval * 1000u;
        }
        if (now_ms >= g_ctx.next_reg_ms) {
            g_ctx.registered = 0;
            send_register(g_ctx.nonce[0] ? 2 : 0);
        }
    } else if (g_ctx.reg_state == 3 && now_ms >= g_ctx.next_reg_ms) {
        g_ctx.reg_auth = 0;
        g_ctx.nonce[0] = '\0';
        send_register(0);
    } else if (g_ctx.reg_state == 0) {
        send_register(0);
    }
}

/* ── MANSCDP 查询/控制 ──────────────────────────────────────────────────── */

static void send_manscdp_to_peer(const gb_sip_msg *m, const char *body)
{
    gb_send_manscdp(m ? m->peer_ip : NULL, m ? m->peer_port : 0, body);
}

typedef struct {
    gb_catalog_item items[ZCS_GB28181_CHANNEL_MAX];
    char ids[ZCS_GB28181_CHANNEL_MAX][ZCS_DEVICE_ID_MAX];
    char names[ZCS_GB28181_CHANNEL_MAX][ZCS_DEVICE_NAME_MAX];
    size_t count;
} gb_catalog_ctx;

static int catalog_visit(const zcs_device_channel_info *ch, void *user)
{
    gb_catalog_ctx *ctx = (gb_catalog_ctx *)user;
    size_t i;

    if (!ch || ctx->count >= ZCS_GB28181_CHANNEL_MAX) {
        return 1;
    }
    i = ctx->count++;
    copy_str(ctx->ids[i], sizeof(ctx->ids[i]), ch->id);
    copy_str(ctx->names[i], sizeof(ctx->names[i]), ch->name);
    ctx->items[i].device_id = ctx->ids[i];
    ctx->items[i].name = ctx->names[i];
    ctx->items[i].manufacturer = g_ctx.manufacturer;
    ctx->items[i].model = g_ctx.model;
    ctx->items[i].parent_id = g_ctx.device_id;
    ctx->items[i].online = ch->online;
    ctx->items[i].ptz_type = (ch->caps & ZCS_DEV_CAP_PTZ) ? 1 : 0;
    return 0;
}

typedef struct {
    zcs_gb28181_channel_info *out;
    size_t cap;
    size_t count;
} gb_chlist_ctx;

static int chlist_visit(const zcs_device_channel_info *ch, void *user)
{
    gb_chlist_ctx *ctx = (gb_chlist_ctx *)user;
    size_t n;

    if (!ch || ctx->count >= ctx->cap) {
        return 1;
    }
    n = ctx->count++;
    copy_str(ctx->out[n].id, sizeof(ctx->out[n].id), ch->id);
    copy_str(ctx->out[n].name, sizeof(ctx->out[n].name), ch->name);
    copy_str(ctx->out[n].vhost, sizeof(ctx->out[n].vhost), ch->vhost);
    copy_str(ctx->out[n].app, sizeof(ctx->out[n].app), ch->app);
    copy_str(ctx->out[n].stream, sizeof(ctx->out[n].stream), ch->stream);
    ctx->out[n].online = ch->online;
    return 0;
}

static int gb_query_catalog(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    /* 目录快照与 XML body 都是几十 KB：堆上分配，别压 poller 栈。 */
    char *body = (char *)malloc(GB_SIP_BODY_MAX);
    gb_catalog_ctx *cat = (gb_catalog_ctx *)calloc(1, sizeof(*cat));

    if (!body || !cat) {
        free(body);
        free(cat);
        return -1;
    }
    /* 查询前先同步实时在线状态：平台查目录时期望看到最新 online 标志，
     * 而不是上次刷新时的快照。 */
    zcs_device_core_sync_live();
    zcs_device_core_foreach(0, ZCS_DEV_CH_GB, catalog_visit, cat);
    gb_manscdp_build_catalog(body, GB_SIP_BODY_MAX, q->sn,
                             q->device_id[0] ? q->device_id : g_ctx.device_id, cat->items,
                             cat->count);
    send_manscdp_to_peer(m, body);
    zn_info("gb28181: catalog query sn=%s channels=%u", q->sn, (unsigned)cat->count);
    free(body);
    free(cat);
    return 0;
}

/* 设备信息查询：上报设备基本参数与通道总数。通道数取 GB 类型实时统计，
 * 保证与目录查询一致。 */
static int gb_query_device_info(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    char body[4096];

    gb_manscdp_build_device_info(body, sizeof(body), q->sn, g_ctx.device_id, g_ctx.device_name,
                                 g_ctx.manufacturer, g_ctx.model, g_ctx.firmware,
                                 (int)zcs_device_core_count(0, ZCS_DEV_CH_GB));
    send_manscdp_to_peer(m, body);
    return 0;
}

/* 设备状态查询：恒上报 online=1（能处理本查询就说明设备在线），SN 原样回填。 */
static int gb_query_device_status(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    char body[2048];

    gb_manscdp_build_device_status(body, sizeof(body), q->sn, g_ctx.device_id, 1);
    send_manscdp_to_peer(m, body);
    return 0;
}

static int gb_query_config(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    char body[4096];
    gb_sb w;

    gb_sb_init(&w, body, sizeof(body));
    gb_sb_puts(&w, "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Response>\r\n");
    gb_sb_printf(&w, "<CmdType>ConfigDownload</CmdType>\r\n<SN>%s</SN>\r\n", q->sn);
    gb_sb_printf(&w, "<DeviceID>%s</DeviceID>\r\n<Result>OK</Result>\r\n", g_ctx.device_id);
    gb_sb_puts(&w, "<BasicParam>\r\n<Name>");
    gb_sb_xml_esc(&w, g_ctx.device_name);
    gb_sb_printf(&w, "</Name>\r\n<Expiration>%d</Expiration>\r\n", g_ctx.register_expires);
    gb_sb_printf(&w, "<HeartBeatInterval>%d</HeartBeatInterval>\r\n",
                 g_ctx.keepalive_interval);
    gb_sb_puts(&w, "<HeartBeatCount>3</HeartBeatCount>\r\n</BasicParam>\r\n</Response>\r\n");
    send_manscdp_to_peer(m, body);
    return 0;
}

static int gb_query_record_info(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    char *body = (char *)malloc(GB_SIP_BODY_MAX);
    gb_record_list *list;
    size_t count;
    size_t i;

    if (!body) {
        return -1;
    }
    list = gb_record_query(q->device_id, q->start_time, q->end_time);
    count = zcs_device_record_list_count(list);
    {
        gb_record_item *items = (gb_record_item *)calloc(count ? count : 1, sizeof(*items));

        if (items) {
            for (i = 0; i < count; ++i) {
                const zcs_device_record_entry *e = zcs_device_record_list_at(list, i);

                items[i].device_id = e->channel_id;
                items[i].name = e->name;
                items[i].file_path = e->file_path;
                items[i].start_time = e->start_time;
                items[i].end_time = e->end_time;
                items[i].type = e->type;
            }
            gb_manscdp_build_record_info(body, GB_SIP_BODY_MAX, q->sn, q->device_id, items, count,
                                         (int)count);
            send_manscdp_to_peer(m, body);
            free(items);
        }
    }
    zn_info("gb28181: record query channel=%s records=%u", q->device_id, (unsigned)count);
    free(body);
    zcs_device_record_list_free(list);
    return 0;
}

/* 预置位查询：本设备不实现预置位，固定回报 Num=0 的空列表。 */
static int gb_query_preset(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    char body[1024];
    gb_sb w;

    gb_sb_init(&w, body, sizeof(body));
    gb_sb_puts(&w, "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Response>\r\n");
    gb_sb_printf(&w, "<CmdType>PresetQuery</CmdType>\r\n<SN>%s</SN>\r\n", q->sn);
    gb_sb_printf(&w, "<DeviceID>%s</DeviceID>\r\n", q->device_id);
    gb_sb_puts(&w,
               "<SumNum>0</SumNum>\r\n<PresetList Num=\"0\"></PresetList>\r\n</Response>\r\n");
    send_manscdp_to_peer(m, body);
    return 0;
}

static const zcs_device_cmd k_gb_queries[] = {
    {"Catalog", "query", 0, gb_query_catalog},
    {"DeviceInfo", "query", 0, gb_query_device_info},
    {"DeviceStatus", "query", 0, gb_query_device_status},
    {"ConfigDownload", "query", 0, gb_query_config},
    {"RecordInfo", "query", ZCS_DEV_CAP_RECORD_QUERY, gb_query_record_info},
    {"PresetQuery", "query", 0, gb_query_preset},
};

/* 手动录像控制：Record 开启并立即起 mp4 录制器；StopRecord 停止。
 * 仅对已注册的通道生效，找不到源就静默返回——录像指向不存在的流没有意义。 */
static void handle_record_cmd(const char *channel_id, const char *cmd)
{
    gb_channel ch;

    if (gb_channel_get(channel_id, &ch) != 0) {
        return;
    }
    if (strcasecmp(cmd, "Record") == 0) {
        zcs_media_source *src = gb_channel_source(&ch);
        if (src) {
            src->enable_mp4 = 1;
            (void)zcs_mp4_recorder_start(src, g_ctx.poller);
            zn_info("gb28181: manual record start channel=%s", channel_id);
        }
    } else if (strcasecmp(cmd, "StopRecord") == 0) {
        zcs_media_source *src = gb_channel_source(&ch);
        if (src) {
            zcs_mp4_recorder_stop(src);
            zn_info("gb28181: manual record stop channel=%s", channel_id);
        }
    }
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

static float clamp_unit(float v)
{
    return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

/*
 * 解析 GB28181 PTZ 控制指令：8 字节十六进制串，b[0..2] 为固定头 A5 0F 01，
 * b[3] 为命令码（方向/变倍/停止），b[4..6] 为 0-100 的 pan/tilt/zoom 速度
 * （除以 100 归一化后 clamp 到 [-1,1]）。方向码的符号约定（pan 右正左负、
 * tilt 上正下负）遵循国标；不认识的命令码返回 -1。
 */
static int parse_ptz_cmd(const char *hex, zcs_device_ptz_cmd *out)
{
    unsigned char b[8];
    size_t i;
    float pan_speed;
    float tilt_speed;
    float zoom_speed;

    if (!hex || strlen(hex) < 16 || !out) {
        return -1;
    }
    for (i = 0; i < 8; ++i) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        b[i] = (unsigned char)((hi << 4) | lo);
    }
    if (b[0] != 0xA5 || b[1] != 0x0F || b[2] != 0x01) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    pan_speed = (float)b[4] / 100.0f;
    tilt_speed = (float)b[5] / 100.0f;
    zoom_speed = (float)b[6] / 100.0f;
    switch (b[3]) {
    case 0x00:
        out->stop = 1;
        break;
    case 0x01:
        out->pan = -pan_speed;
        break;
    case 0x02:
        out->pan = pan_speed;
        break;
    case 0x03:
        out->tilt = tilt_speed;
        break;
    case 0x04:
        out->tilt = -tilt_speed;
        break;
    case 0x05:
        out->pan = -pan_speed;
        out->tilt = tilt_speed;
        break;
    case 0x06:
        out->pan = pan_speed;
        out->tilt = tilt_speed;
        break;
    case 0x07:
        out->pan = -pan_speed;
        out->tilt = -tilt_speed;
        break;
    case 0x08:
        out->pan = pan_speed;
        out->tilt = -tilt_speed;
        break;
    case 0x09:
        out->zoom = zoom_speed;
        break;
    case 0x0A:
        out->zoom = -zoom_speed;
        break;
    default:
        return -1;
    }
    out->pan = clamp_unit(out->pan);
    out->tilt = clamp_unit(out->tilt);
    out->zoom = clamp_unit(out->zoom);
    return 0;
}

/* 设备控制统一入口：PTZ、录像、报警布撤防各自解析后分发；不论各子命令是否
 * 被支持，都统一回一条 DeviceControl Response=OK（GB28181 的设备控制只要求
 * 一个总响应），具体不支持的命令在处理内部记日志。 */
static int gb_control_device(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    char body[1024];

    if (q->ptz_cmd[0]) {
        zcs_device_ptz_cmd cmd;
        if (parse_ptz_cmd(q->ptz_cmd, &cmd) == 0) {
            snprintf(cmd.channel_id, sizeof(cmd.channel_id), "%s", q->device_id);
            (void)zcs_device_ptz_dispatch(&cmd);
        } else {
            zn_info("gb28181: PTZ channel=%s unsupported cmd=%s", q->device_id, q->ptz_cmd);
        }
    }
    if (q->record_cmd[0]) {
        handle_record_cmd(q->device_id, q->record_cmd);
    }
    if (q->guard_cmd[0]) {
        zn_info("gb28181: GuardCmd channel=%s cmd=%s", q->device_id, q->guard_cmd);
    }
    gb_manscdp_build_device_control_resp(body, sizeof(body), q->sn, q->device_id, "OK");
    send_manscdp_to_peer(m, body);
    return 0;
}

static int gb_control_config(const zcs_device_cmd_ctx *ctx)
{
    const gb_sip_msg *m = (const gb_sip_msg *)ctx->session;
    const gb_manscdp_msg *q = (const gb_manscdp_msg *)ctx->message;
    char body[1024];
    gb_sb w;

    gb_sb_init(&w, body, sizeof(body));
    gb_sb_puts(&w, "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Response>\r\n");
    gb_sb_printf(&w, "<CmdType>DeviceConfig</CmdType>\r\n<SN>%s</SN>\r\n", q->sn);
    gb_sb_printf(&w, "<DeviceID>%s</DeviceID>\r\n<Result>OK</Result>\r\n</Response>\r\n",
                 q->device_id);
    send_manscdp_to_peer(m, body);
    return 0;
}

static const zcs_device_cmd k_gb_controls[] = {
    {"DeviceControl", "control", 0, gb_control_device},
    {"DeviceConfig", "control", 0, gb_control_config},
};

#define GB_CMD_COUNT(t) (sizeof(t) / sizeof((t)[0]))

/* 按 CmdType 在查询表里查处理器；能力不满足（如设备未启用录像查询）时只记
 * 日志、不回应——GB28181 的查询是单向通知，静默忽略不支持的查询比回错误码
 * 更符合平台预期（避免平台判定设备异常）。 */
void gb_handle_query(const gb_sip_msg *m, const gb_manscdp_msg *q)
{
    const zcs_device_cmd *cmd = zcs_device_cmd_find(k_gb_queries, GB_CMD_COUNT(k_gb_queries),
                                                    q->cmd_type);
    zcs_device_cmd_ctx ctx;

    if (!cmd || !zcs_device_cmd_allowed(cmd)) {
        zn_info("gb28181: unsupported query CmdType=%s", q->cmd_type);
        return;
    }
    ctx.session = (void *)m;
    ctx.message = q;
    ctx.extra = NULL;
    (void)cmd->exec(&ctx);
}

void gb_handle_control(const gb_sip_msg *m, const gb_manscdp_msg *q)
{
    const zcs_device_cmd *cmd = zcs_device_cmd_find(k_gb_controls, GB_CMD_COUNT(k_gb_controls),
                                                    q->cmd_type);
    zcs_device_cmd_ctx ctx;

    if (!cmd || !zcs_device_cmd_allowed(cmd)) {
        zn_info("gb28181: unsupported control CmdType=%s", q->cmd_type);
        return;
    }
    ctx.session = (void *)m;
    ctx.message = q;
    ctx.extra = NULL;
    (void)cmd->exec(&ctx);
}

/* ── SIP 分发 ───────────────────────────────────────────────────────────── */

static void handle_message(const gb_sip_msg *m)
{
    gb_manscdp_msg q;

    /* 先立即回 200 OK，再处理 MANSCDP 查询/控制：MESSAGE 是 SIP 事务，平台
     * 只关心“收到了”，实际业务结果通过后续的 MANSCDP 响应（另行发送）返回。
     * 先回 200 能避免业务处理耗时导致平台事务超时重传。 */
    gb_send_response(m, 200, "OK", NULL, NULL, NULL, NULL, 0);
    if (m->body_len == 0) {
        return;
    }
    if (gb_manscdp_parse(m->body, m->body_len, &q) != 0) {
        zn_warn("gb28181: bad MANSCDP body from %s:%u", m->peer_ip, m->peer_port);
        return;
    }
    if (strcmp(q.root, "Query") == 0) {
        gb_handle_query(m, &q);
    } else if (strcmp(q.root, "Control") == 0) {
        gb_handle_control(m, &q);
    } else if (strcmp(q.root, "Response") == 0) {
        zn_info("gb28181: device control response sn=%s result=%s", q.sn, q.result);
    }
}

void gb_sip_dispatch(const gb_sip_msg *m)
{
    if (!m) {
        return;
    }
    /* 记住最近一次对端地址：平台由 INVITE/MESSAGE 主动联系我们时源端口可能是
     * 临时的，后续主动上报（保活/告警）优先复用这个已知可达的 peer。 */
    if (m->peer_ip[0]) {
        copy_str(g_ctx.last_peer_ip, sizeof(g_ctx.last_peer_ip), m->peer_ip);
        g_ctx.last_peer_port = m->peer_port;
    }
    if (m->is_response) {
        const char *cseq = gb_sip_hdr(m, "CSeq");
        if (cseq && strstr(cseq, "REGISTER")) {
            handle_register_response(m);
        }
        return;
    }
    if (strcmp(m->method, "MESSAGE") == 0) {
        handle_message(m);
    } else if (strcmp(m->method, "INVITE") == 0) {
        gb_invite_handle(m);
    } else if (strcmp(m->method, "ACK") == 0) {
        gb_invite_handle_ack(m);
    } else if (strcmp(m->method, "BYE") == 0) {
        gb_invite_handle_bye(m);
    } else if (strcmp(m->method, "INFO") == 0) {
        gb_invite_handle_info(m);
    } else if (strcmp(m->method, "OPTIONS") == 0 || strcmp(m->method, "SUBSCRIBE") == 0 ||
               strcmp(m->method, "REGISTER") == 0) {
        gb_send_response(m, 200, "OK", NULL, NULL, NULL, NULL, 0);
    } else {
        gb_send_response(m, 405, "Method Not Allowed", NULL, NULL, NULL, NULL, 0);
    }
}

/* ── 传输回调 / 定时器 ─────────────────────────────────────────────────── */

static void sip_on_message(void *user, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len)
{
    /* 单条消息 ~50KB：放堆上，避免把 poller 线程栈压到百 KB 级。 */
    gb_sip_msg *m = (gb_sip_msg *)calloc(1, sizeof(*m));

    (void)user;
    if (!m) {
        return;
    }
    if (gb_sip_parse(data, len, peer_ip, peer_port, m) != 0) {
        zn_warn("gb28181: bad SIP message from %s:%u (%u bytes)", peer_ip, peer_port,
                 (unsigned)len);
        free(m);
        return;
    }
    gb_sip_dispatch(m);
    free(m);
}

/* TCP 连接状态变化：连上（含重连）后刷新本地端口并立刻排一次注册。
 * 断开时不做事——传输层自身会重连，重连成功再次进入这里。 */
static void on_sip_state(void *user, int connected)
{
    uint16_t port;

    (void)user;
    if (!connected) {
        return;
    }
    port = zcs_sip_transport_local_port(g_ctx.sip);
    if (port) {
        g_ctx.local_port = port;
    }
    /* TCP 重连后重新注册 */
    g_ctx.registered = 0;
    g_ctx.tx_pending = 0;
    g_ctx.reg_state = 3;
    g_ctx.next_reg_ms = zn_monotonic_ms();
}

static void on_tick(void *user)
{
    (void)user;
    gb_register_tick(zn_monotonic_ms());
}

/* ── 生命周期 ───────────────────────────────────────────────────────────── */

int zcs_gb28181_start(const zcs_gb28181_opts *opts)
{
    zn_poller_pool_opts_t popts;
    zcs_sip_transport_opts sopts;
    const char *platform_ip;
    const char *mode;

    if (!opts || !opts->device_id || !opts->device_id[0]) {
        return -1;
    }
    if (g_ctx.running) {
        zcs_gb28181_stop();
    }
    memset(&g_ctx, 0, sizeof(g_ctx));

    copy_str(g_ctx.platform_id, sizeof(g_ctx.platform_id), opts->platform_id);
    copy_str(g_ctx.platform_domain, sizeof(g_ctx.platform_domain),
             opts->platform_domain && opts->platform_domain[0] ? opts->platform_domain
                                                               : opts->platform_id);
    copy_str(g_ctx.platform_ip, sizeof(g_ctx.platform_ip), opts->platform_ip);
    g_ctx.platform_port = opts->platform_port ? opts->platform_port : 5060;
    copy_str(g_ctx.device_id, sizeof(g_ctx.device_id), opts->device_id);
    copy_str(g_ctx.device_name, sizeof(g_ctx.device_name),
             opts->device_name ? opts->device_name : opts->device_id);
    copy_str(g_ctx.manufacturer, sizeof(g_ctx.manufacturer),
             opts->manufacturer ? opts->manufacturer : "Zero");
    copy_str(g_ctx.model, sizeof(g_ctx.model), opts->model ? opts->model : "ZCS");
    copy_str(g_ctx.firmware, sizeof(g_ctx.firmware),
             opts->firmware ? opts->firmware : "0.5.0");
    copy_str(g_ctx.username, sizeof(g_ctx.username), opts->username);
    copy_str(g_ctx.password, sizeof(g_ctx.password), opts->password);
    g_ctx.local_port = opts->local_port ? opts->local_port : 5060;
    g_ctx.register_expires = opts->register_expires > 0 ? opts->register_expires : 3600;
    g_ctx.keepalive_interval = opts->keepalive_interval > 0 ? opts->keepalive_interval : 60;
    copy_str(g_ctx.record_root, sizeof(g_ctx.record_root),
             opts->record_root && opts->record_root[0] ? opts->record_root
                                                       : zcs_mp4_recorder_root());
    zcs_device_core_set_record_root(g_ctx.record_root);

    platform_ip = g_ctx.platform_ip;
    /* local_ip 未显式配置（或为 0.0.0.0）时动态探测出口地址，写入 Via/Contact/SDP。 */
    if (opts->local_ip && opts->local_ip[0] && strcmp(opts->local_ip, "0.0.0.0") != 0) {
        copy_str(g_ctx.local_ip, sizeof(g_ctx.local_ip), opts->local_ip);
    } else {
        detect_local_ip(platform_ip, g_ctx.local_ip, sizeof(g_ctx.local_ip));
    }

    if (!g_ctx.platform_ip[0] || !g_ctx.platform_id[0]) {
        zn_error("gb28181: platform_id/platform_ip required");
        return -1;
    }

    gb_channels_load(opts->channels);

    g_ctx.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!g_ctx.mtx) {
        return -1;
    }

    popts = (zn_poller_pool_opts_t){
        .size = 1,
        .prefer_current_thread = 0,
        .thread_priority = ZB_THREAD_PRIO_NORMAL,
    };
    g_ctx.pool = zn_poller_pool_create(&popts);
    if (!g_ctx.pool || zn_poller_pool_start(g_ctx.pool) != ZN_OK) {
        zn_error("gb28181: poller pool start failed");
        goto fail;
    }
    g_ctx.poller = zn_poller_pool_get_first(g_ctx.pool);
    if (!g_ctx.poller) {
        goto fail;
    }

    mode = opts->sip_transport && opts->sip_transport[0] ? opts->sip_transport : "udp";
    if (strcasecmp(mode, "tcp") == 0) {
        g_ctx.sip_transport_mode = ZCS_SIP_TRANSPORT_TCP;
    } else if (strcasecmp(mode, "tls") == 0) {
        g_ctx.sip_transport_mode = ZCS_SIP_TRANSPORT_TLS;
    } else if (strcasecmp(mode, "udp") == 0) {
        g_ctx.sip_transport_mode = ZCS_SIP_TRANSPORT_UDP;
    } else {
        zn_error("gb28181: unsupported sip_transport=%s (udp/tcp/tls)", mode);
        goto fail;
    }
    {
        const char *smode = opts->sip_mode && opts->sip_mode[0] ? opts->sip_mode : "client";
        if (strcasecmp(smode, "server") == 0) {
            if (g_ctx.sip_transport_mode == ZCS_SIP_TRANSPORT_UDP) {
                zn_error("gb28181: sip_mode=server requires tcp/tls");
                goto fail;
            }
            g_ctx.sip_server_mode = 1;
        } else if (strcasecmp(smode, "client") != 0) {
            zn_error("gb28181: unsupported sip_mode=%s (client/server)", smode);
            goto fail;
        }
    }
    memset(&sopts, 0, sizeof(sopts));
    sopts.poller = g_ctx.poller;
    sopts.poller_pool = g_ctx.pool;
    sopts.server_mode = g_ctx.sip_server_mode;
    sopts.bind_host = "0.0.0.0";
    sopts.local_port = (uint16_t)g_ctx.local_port;
    sopts.mode = g_ctx.sip_transport_mode;
    sopts.remote_host = g_ctx.platform_ip;
    sopts.remote_port = (uint16_t)g_ctx.platform_port;
    sopts.tls_ca = opts->tls_ca;
    sopts.tls_cert = opts->tls_cert;
    sopts.tls_key = opts->tls_key;
    sopts.tls_verify_peer = opts->tls_verify_peer;
    sopts.on_message = sip_on_message;
    sopts.on_state = on_sip_state;
    g_ctx.sip = zcs_sip_transport_create(&sopts);
    if (!g_ctx.sip || zcs_sip_transport_start(g_ctx.sip) != 0) {
        zn_error("gb28181: SIP %s transport start failed", mode);
        goto fail;
    }
    if (g_ctx.sip_transport_mode == ZCS_SIP_TRANSPORT_UDP) {
        g_ctx.local_port = zcs_sip_transport_local_port(g_ctx.sip);
    }

    g_ctx.tick = zn_timer_start(g_ctx.poller, 200, 1, on_tick, NULL);
    if (!g_ctx.tick) {
        goto fail;
    }

    g_ctx.running = 1;
    gb_register_start();
    zcs_device_core_sync_live();
    zn_info("gb28181: device %s SIP/%s %s %s:%u -> platform %s:%u, channels=%u", g_ctx.device_id,
             gb_transport_name(), g_ctx.sip_server_mode ? "server" : "client", g_ctx.local_ip,
             g_ctx.local_port, g_ctx.platform_ip, g_ctx.platform_port,
             (unsigned)zcs_device_core_count(0, ZCS_DEV_CH_GB));
    return 0;

fail:
    if (g_ctx.tick) {
        zn_timer_stop(g_ctx.tick);
        g_ctx.tick = NULL;
    }
    if (g_ctx.sip) {
        zcs_sip_transport_destroy(g_ctx.sip);
        g_ctx.sip = NULL;
    }
    if (g_ctx.pool) {
        zn_poller_pool_stop(g_ctx.pool);
        zn_poller_pool_destroy(g_ctx.pool);
        g_ctx.pool = NULL;
    }
    if (g_ctx.mtx) {
        zb_mutex_destroy(g_ctx.mtx);
        g_ctx.mtx = NULL;
    }
    return -1;
}

/* 关闭顺序：先注销（尽力告知平台）→ 停所有媒体会话 → 停定时器 → 销毁 SIP
 * 传输 → 停 poller 池 → 销毁互斥量。media 先于传输停，保证收尾时还能发
 * BYE/RTP；poller 最后停，确保前面各步的定时器/回调都已完成。 */
void zcs_gb28181_stop(void)
{
    if (!g_ctx.running) {
        return;
    }
    gb_register_stop();
    gb_media_stop_all();
    if (g_ctx.tick) {
        zn_timer_stop(g_ctx.tick);
        g_ctx.tick = NULL;
    }
    if (g_ctx.sip) {
        zcs_sip_transport_destroy(g_ctx.sip);
        g_ctx.sip = NULL;
    }
    if (g_ctx.pool) {
        zn_poller_pool_stop(g_ctx.pool);
        zn_poller_pool_destroy(g_ctx.pool);
        g_ctx.pool = NULL;
        g_ctx.poller = NULL;
    }
    if (g_ctx.mtx) {
        zb_mutex_destroy(g_ctx.mtx);
        g_ctx.mtx = NULL;
    }
    g_ctx.running = 0;
    g_ctx.registered = 0;
    zn_info("gb28181: stopped");
}

int zcs_gb28181_running(void)
{
    return g_ctx.running;
}

int zcs_gb28181_registered(void)
{
    return g_ctx.registered;
}

size_t zcs_gb28181_channels(zcs_gb28181_channel_info *out, size_t cap)
{
    gb_chlist_ctx ctx;

    if (!out || cap == 0) {
        return 0;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.cap = cap;
    zcs_device_core_sync_live();
    zcs_device_core_foreach(0, ZCS_DEV_CH_GB, chlist_visit, &ctx);
    return ctx.count;
}

int zcs_gb28181_session_count(void)
{
    return gb_media_session_count();
}

int zcs_gb28181_alarm(const char *channel_id, int priority, int method, const char *description)
{
    char body[1024];
    char sn[24];
    uint32_t v;

    /* 未注册时主动上报无处可达：平台的告警订阅关系尚未建立，直接失败比
     * 静默发出不存在的事务更诚实。优先复用最近 peer 地址（见 gb_send_manscdp）。 */
    if (!g_ctx.running || !g_ctx.registered) {
        return -1;
    }
    if (g_ctx.mtx) {
        zb_mutex_lock(g_ctx.mtx);
    }
    v = next_sn_locked();
    if (g_ctx.mtx) {
        zb_mutex_unlock(g_ctx.mtx);
    }
    snprintf(sn, sizeof(sn), "%u", v);
    gb_manscdp_build_alarm(body, sizeof(body), sn,
                           channel_id && channel_id[0] ? channel_id : g_ctx.device_id, priority,
                           method, description);
    {
        zcs_device_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ZCS_DEV_EVENT_ALARM;
        snprintf(ev.device_id, sizeof(ev.device_id), "%s", g_ctx.device_id);
        snprintf(ev.channel_id, sizeof(ev.channel_id), "%s",
                 channel_id && channel_id[0] ? channel_id : g_ctx.device_id);
        ev.priority = priority;
        snprintf(ev.message, sizeof(ev.message), "%s", description ? description : "alarm");
        zcs_device_event_publish(&ev);
    }
    return gb_send_manscdp(NULL, 0, body);
}
