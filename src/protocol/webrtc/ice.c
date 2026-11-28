/**
 * @file ice.c
 * @brief WebRTC ICE：候选、连通性检查与发送。
 *
 * 本文件有两个编译分支：
 *   - ZCS_WEBRTC_USE_ZICE：完整 ICE（zice/STUN agent），支持 host/srflx/relay 候选、
 *     连通性检查与 TURN。用于公网/复杂 NAT。
 *   - 否则走“lite ICE”：无 zice，仅靠 host 候选 + 首个 UDP 包学习 peer 地址，
 *     适合 LAN/直连 WHEP。两个分支导出同一套符号，接口一致。
 *
 * 并发约定：ICE 定时器通过 zcs_webrtc_ice_timer_poller 绑定到会话所属 poller，
 * 所有回调都在该 poller 线程执行；本文件用一张全局 agent→poller 表把 agent
 * 反查回 poller（zice 回调只给了 agent/checklist 指针）。
 */
#if defined(ZCS_WEBRTC_USE_ZICE) && ZCS_WEBRTC_USE_ZICE

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#define webrtc_strcasecmp _stricmp
#else
#include <arpa/inet.h>
#include <strings.h>
#define webrtc_strcasecmp strcasecmp
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protocol/webrtc/ice_internal.h"
#include "protocol/webrtc/ice_host.h"
#include "protocol/webrtc/media_internal.h"
#include "zcs/session/webrtc/webrtc_server.h"
#include "zcs/media/media_limits.h"
#include "zcs/media/wire/rtp_packet.h"
#include "zice/ice-agent.h"
#include "zice/stun-agent.h"
#include "zb/os/socket.h"
#include "stun-internal.h"
#include "zb/list.h"
#include "zn/poller/poller.h"
#include "zn/platform.h"
#include "zn/util/log.h"

typedef struct {
    struct ice_agent_t *agent;
    zn_poller *poller;
} webrtc_ice_agent_pol;

/* agent→poller 反查表：zice 的连通性检查回调只拿到 agent/checklist 指针，但定时器
 * 必须挂在正确的 poller 上运行。这张全局表（有上限 ZCS_WEBRTC_ICE_AGENT_MAX）就是
 * 那座桥。注册满时只告警不崩——对应会话会退化为无定时投递。数组访问无锁，因为
 * 注册/注销都在会话 poller 线程、且不同会话的 agent 互不重叠（见文件头并发约定）。 */
static webrtc_ice_agent_pol g_ice_agent_pol[ZCS_WEBRTC_ICE_AGENT_MAX];
static int g_ice_agent_pol_n;

static void webrtc_ice_agent_pol_register(struct ice_agent_t *agent, zn_poller *pol)
{
    int i;

    if (!agent || !pol) {
        return;
    }
    for (i = 0; i < g_ice_agent_pol_n; ++i) {
        if (g_ice_agent_pol[i].agent == agent) {
            g_ice_agent_pol[i].poller = pol;
            return;
        }
    }
    if (g_ice_agent_pol_n < (int)ZCS_WEBRTC_ICE_AGENT_MAX) {
        g_ice_agent_pol[g_ice_agent_pol_n++] = (webrtc_ice_agent_pol){agent, pol};
    } else {
        zn_warn("[webrtc] ICE agent poller map full (max=%u)", (unsigned)ZCS_WEBRTC_ICE_AGENT_MAX);
    }
}

static void webrtc_ice_agent_pol_unregister(struct ice_agent_t *agent)
{
    int i;

    if (!agent) {
        return;
    }
    for (i = 0; i < g_ice_agent_pol_n; ++i) {
        if (g_ice_agent_pol[i].agent == agent) {
            g_ice_agent_pol[i] = g_ice_agent_pol[--g_ice_agent_pol_n];
            return;
        }
    }
}

static zn_poller *webrtc_ice_agent_pol_lookup(struct ice_agent_t *agent)
{
    int i;

    if (!agent) {
        return NULL;
    }
    for (i = 0; i < g_ice_agent_pol_n; ++i) {
        if (g_ice_agent_pol[i].agent == agent) {
            return g_ice_agent_pol[i].poller;
        }
    }
    return NULL;
}

struct webrtc_ice_checklist_view {
    /* 必须与 zice ice-checklist.c 中 ice_checklist_t 头部一致 */
    int32_t ref;
    zb_locker_t locker;
    struct ice_agent_t *ice;
};

/* 从 checklist 裸指针安全还原出 agent。checklist 与 zice 内部结构首部布局耦合
 * （ref/locker/ice），见 webrtc_ice_checklist_view。还原后必须再查 poller 表确认
 * 此 agent 仍存活，否则视为野指针返回 NULL——这一步是防止 use-after-free 的关键。 */
static struct ice_agent_t *webrtc_ice_agent_from_checklist_ptr(void *checklist)
{
    struct webrtc_ice_checklist_view *l = (struct webrtc_ice_checklist_view *)checklist;
    zn_poller *pol;

    if (!l || !l->ice) {
        return NULL;
    }
    /* 仅当 ice 已注册到 poller 表时才采信，避免野指针 */
    pol = webrtc_ice_agent_pol_lookup(l->ice);
    if (!pol) {
        return NULL;
    }
    return l->ice;
}

zn_poller *zcs_webrtc_ice_timer_poller(void *param)
{
    struct ice_agent_t *agent;

    if (!param) {
        return NULL;
    }
    /*
     * ice_checklist_start 传入的是 ice_checklist_t*。
     * 旧逻辑把 param 强转 stun_request_t，再解引用 req->param，
     * 在 Windows 上会偶发 0xc0000005（WHEP SDP ready 后立刻 FAULT）。
     * 这里只按 checklist 布局取 ice，lookup 失败则回退 g_ice_poller。
     */
    agent = webrtc_ice_agent_from_checklist_ptr(param);
    if (agent) {
        return webrtc_ice_agent_pol_lookup(agent);
    }
    return NULL;
}

struct zcs_webrtc_ice {
    zcs_webrtc_ice_host *host;
    struct ice_agent_t *agent;
    struct sockaddr_storage local_addr;
    socklen_t local_len;
    int connected;
    int remote_count;
};

static int webrtc_ice_sockaddr_from_hostport(const char *ip, uint16_t port,
                                             struct sockaddr_storage *out, socklen_t *out_len)
{
    if (!ip || !out || !out_len) {
        return -1;
    }
    return zb_socket_addr_from(out, out_len, ip, port);
}

/* 解析 STUN/TURN 服务器 URL。只支持 UDP：turns:（TLS）与 transport=tcp/tls 返回
 * -2 让调用方计数并跳过（不阻断其它服务器）。缺省端口 3478。 */
static int webrtc_ice_parse_server_url(const char *url, struct sockaddr_storage *out,
                                       socklen_t *out_len)
{
    char host[128];
    const char *p;
    const char *colon;
    const char *q;
    unsigned long port = 3478;

    if (!url || !url[0] || !out || !out_len) {
        return -1;
    }
    p = url;
    if (strncmp(p, "stun:", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "turn:", 5) == 0) {
        p += 5;
    } else if (strncmp(p, "turns:", 6) == 0) {
        return -2; /* TURN over TLS 暂未实现 */
    }
    q = strchr(p, '?');
    if (q) {
        if (strstr(q, "transport=tcp") || strstr(q, "transport=tls")) {
            return -2; /* TURN over TCP/TLS 暂未实现 */
        }
        q = NULL; /* only host:port part; query (transport) ignored */
    }
    colon = strrchr(p, ':');
    if (!colon) {
        snprintf(host, sizeof(host), "%s", p);
    } else {
        size_t n = (size_t)(colon - p);
        if (n == 0 || n >= sizeof(host)) {
            return -1;
        }
        memcpy(host, p, n);
        host[n] = '\0';
        port = strtoul(colon + 1, NULL, 10);
        if (port == 0 || port > 65535) {
            port = 3478;
        }
    }
    return webrtc_ice_sockaddr_from_hostport(host, (uint16_t)port, out, out_len);
}

static void webrtc_ice_peer_from_sockaddr(zcs_webrtc_ice_host *h, const struct sockaddr *sa)
{
    char ip[ZB_SOCKET_ADDRLEN];
    u_short port;

    if (!h || !sa) {
        return;
    }
    if (zb_socket_addr_to(sa, zb_socket_addr_len(sa), ip, &port) != 0) {
        return;
    }
    strncpy(h->peer_ip, ip, sizeof(h->peer_ip) - 1);
    h->peer_port = port;
    h->peer_known = 1;
}

/* zice 的发送出口：把 agent 要发的包（STUN 或经 TURN 的媒体）落到本地 UDP socket。
 * 地址转成 ip/port 后交给 udp_server，未就绪（socket 未绑）直接失败。 */
static int webrtc_ice_onsend(void *param, int protocol, const struct sockaddr *local,
                             const struct sockaddr *remote, const void *data, int bytes)
{
    zcs_webrtc_ice_host *h = (zcs_webrtc_ice_host *)param;
    char ip[ZB_SOCKET_ADDRLEN];
    u_short port;

    (void)protocol;
    (void)local;
    if (!h || !remote || !data || bytes <= 0 || !h->udp) {
        return -1;
    }
    if (zb_socket_addr_to(remote, zb_socket_addr_len(remote), ip, &port) != 0) {
        return -1;
    }
    if (zn_udp_server_sendto(h->udp, ip, port, data, (size_t)bytes) != ZN_OK) {
        return -1;
    }
    return 0;
}

/* 媒体数据到达回调：刷新 peer 地址后转交会话层。peer 从当前选中的远端候选读取，
 * 保证经过 NAT 重绑定（prflx）后仍能更新到真实对端地址。 */
static void webrtc_ice_ondata(void *param, uint8_t stream, uint16_t component, const void *data,
                              int bytes)
{
    zcs_webrtc_ice_host *h = (zcs_webrtc_ice_host *)param;
    struct ice_candidate_t rc;

    (void)stream;
    (void)component;
    if (!h || !data || bytes <= 0) {
        return;
    }
    if (h->ctx && h->ctx->agent &&
        ice_agent_get_remote_candidate(h->ctx->agent, 0, 1, &rc) == 0) {
        webrtc_ice_peer_from_sockaddr(h, (const struct sockaddr *)&rc.addr);
    }
    if (h->on_data) {
        h->on_data(h->owner, data, (size_t)bytes);
    }
}

/*
 * 连通性建立回调。flags bit0=connected。只在首次连上时回调 on_connected 一次
 * （connected 已置位则直接返回），因为会话层据此切换 DTLS 角色/启动发布，重复
 * 触发会重复启动。连上时顺便把实际选中的 remote candidate 记成 peer 地址，
 * 之后媒体可直接 sendto，不必每包再问 agent。
 */
static void webrtc_ice_onconnected(void *param, uint64_t flags, uint64_t mask)
{
    zcs_webrtc_ice_host *h = (zcs_webrtc_ice_host *)param;
    struct ice_candidate_t rc;

    (void)mask;
    if (!h || !h->ctx) {
        return;
    }
    if (flags & 1u) {
        if (h->ctx->connected) {
            return;
        }
        h->ctx->connected = 1;
        if (h->ctx->agent && ice_agent_get_remote_candidate(h->ctx->agent, 0, 1, &rc) == 0) {
            webrtc_ice_peer_from_sockaddr(h, (const struct sockaddr *)&rc.addr);
        }
        zn_info("[webrtc] ICE connected id=%s peer=%s:%u", h->id ? h->id : "?", h->peer_ip,
                 (unsigned)h->peer_port);
        /* 后续动作（DTLS 客户端角色/发布端）由会话层决定。 */
        if (h->on_connected) {
            h->on_connected(h->owner);
        }
    }
}

static void webrtc_ice_ongather(void *param, int code)
{
    zcs_webrtc_ice_host *h = (zcs_webrtc_ice_host *)param;
    if (h) {
        char cand[512];
        size_t n;

        if (h->gather_done < h->gather_expected) {
            ++h->gather_done;
        }
        n = h->ctx ? zcs_webrtc_ice_sdp_candidates(h->ctx, cand, sizeof(cand)) : 0;
        zn_info("[webrtc] ICE gather done id=%s code=%d (%d/%d) extra_candidates=%zu",
                 h->id ? h->id : "?", code, h->gather_done, h->gather_expected, n);
    }
}

static int webrtc_ice_proto_from_transport(const char *transport)
{
    if (!transport) {
        return STUN_PROTOCOL_UDP;
    }
    if (webrtc_strcasecmp(transport, "UDP") == 0) {
        return STUN_PROTOCOL_UDP;
    }
    if (webrtc_strcasecmp(transport, "TCP") == 0) {
        return STUN_PROTOCOL_TCP;
    }
    if (webrtc_strcasecmp(transport, "TLS") == 0) {
        return STUN_PROTOCOL_TLS;
    }
    if (webrtc_strcasecmp(transport, "DTLS") == 0) {
        return STUN_PROTOCOL_DTLS;
    }
    return STUN_PROTOCOL_UDP;
}

static enum ice_candidate_type_t webrtc_ice_type_from_name(const char *name)
{
    if (!name) {
        return ICE_CANDIDATE_HOST;
    }
    if (strcmp(name, "host") == 0) {
        return ICE_CANDIDATE_HOST;
    }
    if (strcmp(name, "srflx") == 0) {
        return ICE_CANDIDATE_SERVER_REFLEXIVE;
    }
    if (strcmp(name, "prflx") == 0) {
        return ICE_CANDIDATE_PEER_REFLEXIVE;
    }
    if (strcmp(name, "relay") == 0) {
        return ICE_CANDIDATE_RELAYED;
    }
    return ICE_CANDIDATE_HOST;
}

struct sdp_ice_candidate_t {
    char foundation[33];
    char transport[8];
    char candtype[8];
    uint16_t component;
    uint16_t port;
    uint32_t priority;
    char address[65];
};

static int webrtc_ice_parse_candidate_value(const char *value, struct sdp_ice_candidate_t *sc)
{
    char typ[16];
    unsigned component;
    unsigned port;
    unsigned priority;

    if (!value || !sc) {
        return -1;
    }
    memset(sc, 0, sizeof(*sc));
    if (sscanf(value, "%32s %u %7s %u %64s %u typ %15s", sc->foundation, &component, sc->transport,
               &priority, sc->address, &port, typ) < 7) {
        return -1;
    }
    sc->component = (uint16_t)component;
    sc->port = (uint16_t)port;
    sc->priority = priority;
    snprintf(sc->candtype, sizeof(sc->candtype), "%s", typ);
    return 0;
}

static int webrtc_ice_add_remote_candidate(struct ice_agent_t *agent,
                                           const struct sdp_ice_candidate_t *sc, uint8_t stream)
{
    struct ice_candidate_t c;
    socklen_t len;

    memset(&c, 0, sizeof(c));
    c.stream = stream;
    c.component = sc->component ? sc->component : 1;
    c.priority = sc->priority ? sc->priority : 1;
    c.protocol = webrtc_ice_proto_from_transport(sc->transport);
    c.type = webrtc_ice_type_from_name(sc->candtype);
    if (sc->foundation[0]) {
        snprintf(c.foundation, sizeof(c.foundation), "%s", sc->foundation);
    }
    if (zb_socket_addr_from(&c.addr, &len, sc->address, sc->port) != 0) {
        return -1;
    }
    memcpy(&c.host, &c.addr, sizeof(c.host));
    return ice_agent_add_remote_candidate(agent, &c);
}

/*
 * 从 SDP（offer 或 answer）里扫出所有 a=candidate: 行并加为远端的候选。
 * 只处理 component 1（RTP），每条解析成功的候选都 add_remote_candidate。
 * 返回成功添加的条数，供日志/统计。
 */
static int webrtc_ice_parse_offer_candidates(struct ice_agent_t *agent, const char *offer,
                                             size_t offer_len)
{
    const char *p = offer;
    const char *end = offer + offer_len;
    int count = 0;

    if (!agent || !offer) {
        return 0;
    }
    while (p < end) {
        const char *line = p;
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - line) : (size_t)(end - line);

        if (line_len > 0 && line[line_len - 1] == '\r') {
            --line_len;
        }
        if (line_len > 12 && strncmp(line, "a=candidate:", 12) == 0) {
            struct sdp_ice_candidate_t sc;

            if (webrtc_ice_parse_candidate_value(line + 12, &sc) == 0 &&
                webrtc_ice_add_remote_candidate(agent, &sc, 0) == 0) {
                ++count;
            }
        }
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    return count;
}

static int webrtc_ice_add_local_host(struct zcs_webrtc_ice *ctx, const char *host, uint16_t port)
{
    struct ice_candidate_t c;
    socklen_t len;

    if (!ctx || !ctx->agent || !host) {
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.stream = 0;
    c.component = 1;
    c.type = ICE_CANDIDATE_HOST;
    c.protocol = STUN_PROTOCOL_UDP;
    snprintf(c.foundation, sizeof(c.foundation), "1");
    if (zb_socket_addr_from(&c.addr, &len, host, port) != 0) {
        return -1;
    }
    memcpy(&c.host, &c.addr, sizeof(c.host));
    ice_candidate_priority(&c);
    memcpy(&ctx->local_addr, &c.addr, sizeof(ctx->local_addr));
    ctx->local_len = len;
    return ice_agent_add_local_candidate(ctx->agent, &c);
}

/* 创建 agent。server 端默认开启 ICE-lite（不主动做连通检查，只被动应答，省资源，
 * 与浏览器做标准 ICE 正好互补）；client 端（WHEP/推流端主动连）则走完整检查。 */
struct zcs_webrtc_ice *zcs_webrtc_ice_create(zcs_webrtc_ice_host *host)
{
    struct zcs_webrtc_ice *ctx;
    struct ice_agent_handler_t handler;

    if (!host) {
        return NULL;
    }
    ctx = (struct zcs_webrtc_ice *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->host = host;
    memset(&handler, 0, sizeof(handler));
    handler.send = webrtc_ice_onsend;
    handler.ondata = webrtc_ice_ondata;
    handler.ongather = webrtc_ice_ongather;
    handler.onconnected = webrtc_ice_onconnected;
    ctx->agent = ice_agent_create(host->client_mode ? 1 : 0, &handler, host);
    if (!ctx->agent) {
        free(ctx);
        return NULL;
    }
    if (!host->client_mode) {
        ice_agent_set_icelite(ctx->agent, 1);
    }
    webrtc_ice_agent_pol_register(ctx->agent, host->poller);
    host->ctx = ctx;
    return ctx;
}

void zcs_webrtc_ice_destroy(struct zcs_webrtc_ice *ice)
{
    if (!ice) {
        return;
    }
    if (ice->host) {
        ice->host->ctx = NULL;
    }
    if (ice->agent) {
        webrtc_ice_agent_pol_unregister(ice->agent);
        ice_agent_stop(ice->agent);
        ice_agent_destroy(ice->agent);
        ice->agent = NULL;
    }
    free(ice);
}

/*
 * 遍历 STUN/TURN 服务器逐个发起 gather。gather_expected 记“应完成的服务器数”，
 * gather_done 由 ongather 回调累加——上层据此判断候选收集是否结束（配齐再发 SDP）。
 * 解析失败/不支持的服务器也要把 gather_done 加一，否则 never 达 expected，SDP 永远发不出去。
 */
static void webrtc_ice_gather_servers(struct zcs_webrtc_ice *ice)
{
    zcs_webrtc_ice_server servers[2];
    size_t n = zcs_webrtc_server_ice_servers(servers, sizeof(servers) / sizeof(servers[0]));
    size_t i;

    if (ice->host) {
        ice->host->gather_expected = (int)n;
        ice->host->gather_done = 0;
    }
    for (i = 0; i < n; ++i) {
        struct sockaddr_storage addr;
        socklen_t addr_len = 0;

        int rc = webrtc_ice_parse_server_url(servers[i].url, &addr, &addr_len);

        if (rc == -2) {
            zn_warn("[webrtc] ICE server transport unsupported (need UDP) id=%s url=%s",
                     ice->host && ice->host->id ? ice->host->id : "?", servers[i].url);
            if (ice->host && ice->host->gather_done < ice->host->gather_expected) {
                ++ice->host->gather_done;
            }
            continue;
        }
        if (rc != 0) {
            zn_warn("[webrtc] ICE server parse failed id=%s url=%s",
                     ice->host && ice->host->id ? ice->host->id : "?", servers[i].url);
            if (ice->host && ice->host->gather_done < ice->host->gather_expected) {
                ++ice->host->gather_done;
            }
            continue;
        }
        (void)ice_agent_gather(ice->agent, (const struct sockaddr *)&addr, servers[i].turn, 1000,
                               servers[i].turn ? STUN_CREDENTIAL_LONG_TERM
                                               : STUN_CREDENTIAL_SHORT_TERM,
                               servers[i].user, servers[i].pass);
    }
    if (n > 0) {
        zn_info("[webrtc] ICE gather start id=%s servers=%zu",
                 ice->host && ice->host->id ? ice->host->id : "?", n);
    }
}

int zcs_webrtc_ice_setup(struct zcs_webrtc_ice *ice, const char *offer, size_t offer_len,
                         const char *local_ufrag, const char *local_pwd, const char *remote_ufrag,
                         const char *remote_pwd, const char *advertise_host, uint16_t local_port)
{
    if (!ice || !ice->agent || !local_ufrag || !local_pwd || !remote_ufrag || !remote_pwd ||
        !advertise_host) {
        return -1;
    }
    zn_info("[webrtc] ICE setup begin id=%s host=%s:%u", ice->host && ice->host->id ? ice->host->id : "?",
             advertise_host, (unsigned)local_port);
    ice_agent_set_local_auth(ice->agent, local_ufrag, local_pwd);
    ice_agent_set_remote_auth(ice->agent, 0, remote_ufrag, remote_pwd);
    if (webrtc_ice_add_local_host(ice, advertise_host, local_port) != 0) {
        zn_warn("[webrtc] ICE add local host failed id=%s host=%s:%u",
                 ice->host && ice->host->id ? ice->host->id : "?", advertise_host, (unsigned)local_port);
        return -1;
    }
    ice->remote_count = webrtc_ice_parse_offer_candidates(ice->agent, offer, offer_len);
    if (ice_agent_start(ice->agent) != 0) {
        zn_warn("[webrtc] ICE agent start failed id=%s remotes=%d",
                 ice->host && ice->host->id ? ice->host->id : "?", ice->remote_count);
        return -1;
    }
    {
        webrtc_ice_gather_servers(ice);
    }
    zn_info("[webrtc] ICE setup id=%s host=%s:%u remotes=%d",
             ice->host && ice->host->id ? ice->host->id : "?", advertise_host, (unsigned)local_port,
             ice->remote_count);
    return 0;
}

int zcs_webrtc_ice_prepare_client(struct zcs_webrtc_ice *ice, const char *local_ufrag,
                                  const char *local_pwd, const char *advertise_host,
                                  uint16_t local_port)
{
    if (!ice || !ice->agent || !local_ufrag || !local_pwd || !advertise_host) {
        return -1;
    }
    ice_agent_set_local_auth(ice->agent, local_ufrag, local_pwd);
    if (webrtc_ice_add_local_host(ice, advertise_host, local_port) != 0) {
        zn_warn("[webrtc] client ICE add local host failed id=%s host=%s:%u",
                 ice->host && ice->host->id ? ice->host->id : "?", advertise_host, (unsigned)local_port);
        return -1;
    }
    webrtc_ice_gather_servers(ice);
    zn_info("[webrtc] client ICE prepare id=%s host=%s:%u",
             ice->host && ice->host->id ? ice->host->id : "?", advertise_host, (unsigned)local_port);
    return 0;
}

int zcs_webrtc_ice_connect_client(struct zcs_webrtc_ice *ice, const char *answer,
                                  size_t answer_len, const char *remote_ufrag,
                                  const char *remote_pwd)
{
    if (!ice || !ice->agent || !answer || !remote_ufrag || !remote_pwd) {
        return -1;
    }
    ice_agent_set_remote_auth(ice->agent, 0, remote_ufrag, remote_pwd);
    ice->remote_count = webrtc_ice_parse_offer_candidates(ice->agent, answer, answer_len);
    if (ice_agent_start(ice->agent) != 0) {
        zn_warn("[webrtc] client ICE start failed id=%s remotes=%d",
                 ice->host && ice->host->id ? ice->host->id : "?", ice->remote_count);
        return -1;
    }
    zn_info("[webrtc] client ICE connect id=%s remotes=%d",
             ice->host && ice->host->id ? ice->host->id : "?", ice->remote_count);
    return 0;
}

int zcs_webrtc_ice_add_remote_sdp(struct zcs_webrtc_ice *ice, const char *sdp, size_t len)
{
    if (!ice || !ice->agent || !sdp || len == 0) {
        return -1;
    }
    return webrtc_ice_parse_offer_candidates(ice->agent, sdp, len);
}

struct webrtc_ice_sdp_ctx {
    char *out;
    size_t cap;
    size_t len;
};

/* 把本地候选拼成 SDP a=candidate 行。跳过 host 类型：host 候选由本地 SDP 的
 * c=/m= 行隐含表达，重复列出会让部分浏览器产生冗余候选；只追加 srflx/relay 等
 * server 侧额外候选。 */
static int webrtc_ice_sdp_candidate_visit(const struct ice_candidate_t *c, void *param)
{
    struct webrtc_ice_sdp_ctx *ctx = (struct webrtc_ice_sdp_ctx *)param;
    char ip[ZB_SOCKET_ADDRLEN];
    u_short port;
    int n;

    if (!ctx || !c || c->type == ICE_CANDIDATE_HOST) {
        return 0;
    }
    if (zb_socket_addr_to((const struct sockaddr *)&c->addr, zb_socket_addr_len((const struct sockaddr *)&c->addr),
                       ip, &port) != 0) {
        return 0;
    }
    n = snprintf(ctx->out + ctx->len, ctx->cap > ctx->len ? ctx->cap - ctx->len : 0,
                 "a=candidate:%s %u udp %u %s %u typ %s generation 0\r\n", c->foundation,
                 (unsigned)c->component, (unsigned)c->priority, ip, (unsigned)port,
                 ice_candidate_typename(c));
    if (n > 0 && (size_t)n < ctx->cap - ctx->len) {
        ctx->len += (size_t)n;
    }
    return 0;
}

size_t zcs_webrtc_ice_sdp_candidates(struct zcs_webrtc_ice *ice, char *out, size_t cap)
{
    struct webrtc_ice_sdp_ctx ctx;

    if (!ice || !ice->agent || !out || cap == 0) {
        return 0;
    }
    ctx.out = out;
    ctx.cap = cap;
    ctx.len = 0;
    out[0] = '\0';
    (void)ice_agent_list_local_candidate(ice->agent, webrtc_ice_sdp_candidate_visit, &ctx);
    return ctx.len;
}

/*
 * UDP 入包分诊：先记录 peer，再判断包类型。
 * DTLS / RTP / RTCP 不是 STUN，zice 的 stun_agent 会把它们当垃圾丢弃，所以这里直接
 * 走媒体路径（webrtc_ice_ondata）；只有真正的 STUN 消息才交给 ice_agent_input 做
 * 连通性检查。local_len==0 表示本地候选还没建立，此时 STUN 无从处理，直接丢弃。
 */
void zcs_webrtc_ice_on_udp(struct zcs_webrtc_ice *ice, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len)
{
    struct sockaddr_storage remote;
    socklen_t remote_len;
    const uint8_t *pkt = (const uint8_t *)data;

    if (!ice || !ice->agent || !pkt || len == 0) {
        return;
    }
    if (peer_ip && peer_ip[0]) {
        if (webrtc_ice_sockaddr_from_hostport(peer_ip, peer_port, &remote, &remote_len) == 0) {
            webrtc_ice_peer_from_sockaddr(ice->host, (const struct sockaddr *)&remote);
        }
    }
    /* zice stun_agent 将 DTLS（content-type 204）当 STUN 丢弃；mux 到媒体路径。 */
    if (zcs_webrtc_packet_is_dtls(pkt, len) || zcs_webrtc_packet_is_rtp(pkt, len) ||
        zcs_rtp_is_rtcp(pkt, len)) {
        webrtc_ice_ondata(ice->host, 0, 1, data, (int)len);
        return;
    }
    if (ice->local_len == 0) {
        return;
    }
    if (peer_ip && peer_ip[0] &&
        webrtc_ice_sockaddr_from_hostport(peer_ip, peer_port, &remote, &remote_len) == 0) {
        (void)ice_agent_input(ice->agent, STUN_PROTOCOL_UDP,
                              (const struct sockaddr *)&ice->local_addr,
                              (const struct sockaddr *)&remote, data, (int)len);
    }
}

int zcs_webrtc_ice_uses_relay(struct zcs_webrtc_ice *ice)
{
    struct ice_candidate_t local;

    if (!ice || !ice->agent) {
        return 0;
    }
    memset(&local, 0, sizeof(local));
    if (ice_agent_get_local_candidate(ice->agent, 0, 1, &local) != 0) {
        return 0;
    }
    return local.type == ICE_CANDIDATE_RELAYED;
}

/*
 * 发送媒体数据，按连接状态选择路径：
 *   1) relay 候选：必须经 TURN（ice_agent_send），直发会被对端 NAT 丢弃；
 *   2) 已连接且 peer 已知：直发 UDP（最快），且**不回退** ice_agent_send——
 *      已连接后走 zice 有 index bug，且媒体突发不该再绕 STUN 路径；
 *   3) 尚未连接：先试 ice_agent_send（可能经候选），再回退直发已知 peer。
 */
int zcs_webrtc_ice_send(struct zcs_webrtc_ice *ice, const void *data, size_t len)
{
    if (!ice || !ice->agent || !data || len == 0) {
        return -1;
    }
    /* relay 候选：媒体必须经 TURN channel/data 发送，不能直接发 peer。 */
    if (zcs_webrtc_ice_uses_relay(ice)) {
        return ice_agent_send(ice->agent, 0, 1, data, (int)len) == 0 ? 0 : -1;
    }
    if (ice->connected && ice->host && ice->host->peer_known && ice->host->udp) {
        if (zn_udp_server_sendto(ice->host->udp, ice->host->peer_ip, ice->host->peer_port, data,
                                  len) == ZN_OK) {
            return 0;
        }
        /* 已连接：勿回退 ice_agent_send（zice index bug + 媒体突发避开 STUN 路径）。 */
        return -1;
    }
    if (ice_agent_send(ice->agent, 0, 1, data, (int)len) == 0) {
        return 0;
    }
    if (ice->host && ice->host->peer_known && ice->host->udp) {
        return zn_udp_server_sendto(ice->host->udp, ice->host->peer_ip, ice->host->peer_port,
                                     data, len) == ZN_OK
                   ? 0
                   : -1;
    }
    return -1;
}

int zcs_webrtc_ice_connected(const struct zcs_webrtc_ice *ice)
{
    return ice && ice->connected;
}

#else /* !ZCS_WEBRTC_USE_ZICE */

#include "protocol/webrtc/ice_internal.h"
#include "protocol/webrtc/media_internal.h"
#include "zn/net/udp_server.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

/** LAN / host-candidate WHEP：无 zice；peer 来自首 UDP，SDP 已带 host candidate。 */
struct zcs_webrtc_ice {
    zcs_webrtc_ice_host *host;
    int connected;
};

void zcs_webrtc_ice_port_init(zn_poller *poller)
{
    (void)poller;
}

void zcs_webrtc_ice_port_fini(void) {}

struct zcs_webrtc_ice *zcs_webrtc_ice_create(zcs_webrtc_ice_host *host)
{
    struct zcs_webrtc_ice *ice;

    if (!host) {
        return NULL;
    }
    ice = (struct zcs_webrtc_ice *)calloc(1, sizeof(*ice));
    if (ice) {
        ice->host = host;
        host->ctx = ice;
    }
    return ice;
}

void zcs_webrtc_ice_destroy(struct zcs_webrtc_ice *ice)
{
    if (!ice) {
        return;
    }
    if (ice->host) {
        ice->host->ctx = NULL;
    }
    free(ice);
}

int zcs_webrtc_ice_setup(struct zcs_webrtc_ice *ice, const char *offer, size_t offer_len,
                         const char *local_ufrag, const char *local_pwd, const char *remote_ufrag,
                         const char *remote_pwd, const char *advertise_host, uint16_t local_port)
{
    (void)offer;
    (void)offer_len;
    (void)local_ufrag;
    (void)local_pwd;
    (void)remote_ufrag;
    (void)remote_pwd;
    if (!ice || !advertise_host) {
        return -1;
    }
    zn_info("[webrtc] lite ICE setup id=%s host=%s:%u (no zice)",
             ice->host && ice->host->id ? ice->host->id : "?", advertise_host,
             (unsigned)local_port);
    return 0;
}

int zcs_webrtc_ice_prepare_client(struct zcs_webrtc_ice *ice, const char *local_ufrag,
                                  const char *local_pwd, const char *advertise_host,
                                  uint16_t local_port)
{
    (void)ice;
    (void)local_ufrag;
    (void)local_pwd;
    (void)advertise_host;
    (void)local_port;
    return -1;
}

int zcs_webrtc_ice_connect_client(struct zcs_webrtc_ice *ice, const char *answer,
                                  size_t answer_len, const char *remote_ufrag,
                                  const char *remote_pwd)
{
    (void)ice;
    (void)answer;
    (void)answer_len;
    (void)remote_ufrag;
    (void)remote_pwd;
    return -1;
}

/* lite 分支的“连通”判定：收到首个 UDP 包即认为连通，并把来源地址固化为 peer。
 * 不做 STUN 检查、不维护候选表——LAN/直连场景对端就是发第一个包的那台机器。
 * 只在首次固化为 connected 时回调 on_connected，语义与 zice 分支保持一致。 */
static void webrtc_lite_ice_peer(zcs_webrtc_ice_host *h, const char *peer_ip, uint16_t peer_port,
                                 struct zcs_webrtc_ice *ice)
{
    if (!h || !peer_ip || !peer_ip[0]) {
        return;
    }
    strncpy(h->peer_ip, peer_ip, sizeof(h->peer_ip) - 1);
    h->peer_port = peer_port;
    h->peer_known = 1;
    if (ice && !ice->connected) {
        ice->connected = 1;
        zn_info("[webrtc] lite ICE connected id=%s peer=%s:%u", h->id ? h->id : "?", h->peer_ip,
                 (unsigned)h->peer_port);
        if (h->on_connected) {
            h->on_connected(h->owner);
        }
    }
}

void zcs_webrtc_ice_on_udp(struct zcs_webrtc_ice *ice, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len)
{
    zcs_webrtc_ice_host *h;
    const uint8_t *pkt = (const uint8_t *)data;

    if (!ice || !ice->host || !pkt || len == 0) {
        return;
    }
    h = ice->host;
    webrtc_lite_ice_peer(h, peer_ip, peer_port, ice);
    if (h->on_data) {
        h->on_data(h->owner, data, len);
    }
}

int zcs_webrtc_ice_send(struct zcs_webrtc_ice *ice, const void *data, size_t len)
{
    zcs_webrtc_ice_host *h;

    if (!ice || !data || len == 0) {
        return -1;
    }
    h = ice->host;
    if (!h || !h->udp || !h->peer_known) {
        return -1;
    }
    return zn_udp_server_sendto(h->udp, h->peer_ip, h->peer_port, data, len) == ZN_OK ? 0 : -1;
}

int zcs_webrtc_ice_connected(const struct zcs_webrtc_ice *ice)
{
    return ice && ice->connected;
}

zn_poller *zcs_webrtc_ice_timer_poller(void *param)
{
    (void)param;
    return NULL;
}

#endif /* ZCS_WEBRTC_USE_ZICE */
