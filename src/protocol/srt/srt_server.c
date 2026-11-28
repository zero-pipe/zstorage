/**
 * @file srt_server.c
 * @brief SRT 监听：MPEG-TS 推流写入 gop_queue。
 */
#include "server_internal.h"
#include "poller.h"
#include "zcs/play/dispatch.h"
#include "zcs/protocol/srt/srt_server.h"
#include "zn/util/log.h"
#include <srt/srt.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

static int g_srt_service_initialized;

static int srt_ensure_startup(void)
{
    if (g_srt_service_initialized) {
        return 0;
    }
    if (srt_startup() != 0) {
        return -1;
    }
    g_srt_service_initialized = 1;
    return 0;
}

int zcs_srt_server_available(void)
{
    return 1;
}

#define ZCS_SRT_LIVE_LATENCY_MS 200
#define ZCS_SRT_LIVE_PAYLOAD 1316 /* 188*7, MPEG-TS over SRT live */
#define ZCS_SRT_RCVBUF_BYTES (24 * 1024 * 1024)

/* 进程内单 SRT 服务；由 [srt] 配置填充，供 listener / accepted socket 共用。 */
typedef struct {
    int latency_ms;
    int rcvbuf;
    int sndbuf;
    int timeout_sec;
    int pkt_buf_size;
    char passphrase[128];
} zcs_srt_runtime_opts;

static zcs_srt_runtime_opts g_srt_rt = {
    ZCS_SRT_LIVE_LATENCY_MS, ZCS_SRT_RCVBUF_BYTES, ZCS_SRT_RCVBUF_BYTES, 5, 8192, {0}};

static void srt_runtime_from_opts(const zcs_srt_server_opts *opts)
{
    int mul;
    int pkt;
    long bytes;

    if (!opts) {
        return;
    }
    mul = opts->latency_mul > 0 ? opts->latency_mul : 4;
    g_srt_rt.latency_ms = mul * 50;
    if (g_srt_rt.latency_ms < 20) {
        g_srt_rt.latency_ms = 20;
    }
    pkt = opts->pkt_buf_size > 0 ? opts->pkt_buf_size : 8192;
    bytes = (long)pkt * (long)ZCS_SRT_LIVE_PAYLOAD;
    if (bytes < 1024 * 1024) {
        bytes = 1024 * 1024;
    }
    g_srt_rt.rcvbuf = (int)bytes;
    g_srt_rt.sndbuf = (int)bytes;
    g_srt_rt.pkt_buf_size = pkt;
    g_srt_rt.timeout_sec = opts->timeout_sec > 0 ? opts->timeout_sec : 5;
    g_srt_rt.passphrase[0] = '\0';
    if (opts->passphrase && opts->passphrase[0]) {
        strncpy(g_srt_rt.passphrase, opts->passphrase, sizeof(g_srt_rt.passphrase) - 1);
    }
}

static void srt_apply_idle_timeout(SRTSOCKET sock)
{
    int idle_ms = g_srt_rt.timeout_sec * 1000;

#ifdef SRTO_PEERIDLETIMEO
    (void)srt_setsockopt(sock, 0, SRTO_PEERIDLETIMEO, &idle_ms, sizeof(idle_ms));
#elif defined(SRTO_PEERIDLETIMEOUT)
    (void)srt_setsockopt(sock, 0, SRTO_PEERIDLETIMEOUT, &idle_ms, sizeof(idle_ms));
#else
    (void)sock;
    (void)idle_ms;
#endif
}

static void srt_apply_listener_opts(SRTSOCKET sock)
{
    int transtype = SRTT_LIVE;
    int latency = g_srt_rt.latency_ms;
    int payload = ZCS_SRT_LIVE_PAYLOAD;
    int rcvbuf = g_srt_rt.rcvbuf;
    int sndbuf = g_srt_rt.sndbuf;
    int tsbpd = 1;
    int tlpktdrop = 1;
    int no = 0;

    if (sock == SRT_INVALID_SOCK) {
        return;
    }
    (void)srt_setsockopt(sock, 0, SRTO_TRANSTYPE, &transtype, sizeof(transtype));
    (void)srt_setsockopt(sock, 0, SRTO_LATENCY, &latency, sizeof(latency));
    (void)srt_setsockopt(sock, 0, SRTO_TSBPDMODE, &tsbpd, sizeof(tsbpd));
    (void)srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
    (void)srt_setsockopt(sock, 0, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    (void)srt_setsockopt(sock, 0, SRTO_SNDBUF, &sndbuf, sizeof(sndbuf));
    (void)srt_setsockopt(sock, 0, SRTO_TLPKTDROP, &tlpktdrop, sizeof(tlpktdrop));
    (void)srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof(no));
    srt_apply_idle_timeout(sock);
    if (g_srt_rt.passphrase[0]) {
        int enforced = 1;

        if (srt_setsockopt(sock, 0, SRTO_PASSPHRASE, g_srt_rt.passphrase,
                           (int)strlen(g_srt_rt.passphrase)) != 0) {
            zn_warn("SRT passPhrase rejected (need 10-79 chars): %s", srt_getlasterror_str());
        }
        (void)srt_setsockopt(sock, 0, SRTO_ENFORCEDENCRYPTION, &enforced, sizeof(enforced));
    }
}

void zcs_srt_apply_session_opts(SRTSOCKET sock)
{
    int payload = ZCS_SRT_LIVE_PAYLOAD;
    int rcvbuf = g_srt_rt.rcvbuf;
    int sndbuf = g_srt_rt.sndbuf;
    int tlpktdrop = 1;
    int no = 0;

    if (sock == SRT_INVALID_SOCK) {
        return;
    }
    /* 握手后勿改 TSBPD/latency（会破坏投递）。 */
    (void)srt_setsockopt(sock, 0, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
    (void)srt_setsockopt(sock, 0, SRTO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    (void)srt_setsockopt(sock, 0, SRTO_SNDBUF, &sndbuf, sizeof(sndbuf));
    (void)srt_setsockopt(sock, 0, SRTO_TLPKTDROP, &tlpktdrop, sizeof(tlpktdrop));
    (void)srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof(no));
    (void)srt_setsockopt(sock, 0, SRTO_SNDSYN, &no, sizeof(no));
    srt_apply_idle_timeout(sock);
}

static int srt_is_any_host(const char *host)
{
    char buf[64];
    size_t n;
    const char *h = host;

    if (!h || !h[0]) {
        return 1;
    }
    if (strcmp(h, "0.0.0.0") == 0 || strcmp(h, "*") == 0) {
        return 1;
    }
    n = strlen(h);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, h, n);
    buf[n] = '\0';
    while (n > 0 &&
           (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = '\0';
    }
    return !buf[0] || strcmp(buf, "0.0.0.0") == 0 || strcmp(buf, "*") == 0;
}

static int srt_parse_ipv4(const char *host, struct in_addr *out)
{
    char buf[64];
    size_t n;
    const char *h = host;

    if (!out || !h || !h[0]) {
        return -1;
    }
    n = strlen(h);
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, h, n);
    buf[n] = '\0';
    while (n > 0 &&
           (buf[n - 1] == ' ' || buf[n - 1] == '\t' || buf[n - 1] == '\r' || buf[n - 1] == '\n')) {
        buf[--n] = '\0';
    }
#if defined(_WIN32)
    if (InetPtonA(AF_INET, buf, out) != 1) {
        return -1;
    }
    return 0;
#else
    return inet_pton(AF_INET, buf, out) == 1 ? 0 : -1;
#endif
}

static SRTSOCKET srt_create_listener(const char *host, uint16_t port)
{
    SRTSOCKET ls;
    struct sockaddr_in sa;
    int yes = 1;

    ls = srt_create_socket();
    if (ls == SRT_INVALID_SOCK) {
        zn_error("SRT create_socket failed: %s", srt_getlasterror_str());
        return SRT_INVALID_SOCK;
    }

    (void)srt_setsockopt(ls, 0, SRTO_REUSEADDR, &yes, sizeof(yes));
    srt_apply_listener_opts(ls);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (srt_is_any_host(host)) {
        sa.sin_addr.s_addr = INADDR_ANY;
    } else if (srt_parse_ipv4(host, &sa.sin_addr) != 0) {
        zn_error("SRT invalid listen host: %s", host ? host : "(null)");
        srt_close(ls);
        return SRT_INVALID_SOCK;
    }

    if (srt_bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == SRT_INVALID_SOCK) {
        zn_error("SRT bind %s:%u failed: %s", host && host[0] ? host : "0.0.0.0", (unsigned)port,
                  srt_getlasterror_str());
        srt_close(ls);
        return SRT_INVALID_SOCK;
    }
    if (srt_listen(ls, 128) == SRT_INVALID_SOCK) {
        zn_error("SRT listen %u failed: %s", (unsigned)port, srt_getlasterror_str());
        srt_close(ls);
        return SRT_INVALID_SOCK;
    }
    return ls;
}

zcs_srt_server *zcs_srt_server_create(const zcs_srt_server_opts *opts)
{
    zcs_srt_server *srv;

    if (!opts || !opts->poller) {
        return NULL;
    }
    if (srt_ensure_startup() != 0) {
        zn_error("SRT startup failed: %s", srt_getlasterror_str());
        return NULL;
    }

    zcs_session_dispatch_register_all();
    srt_runtime_from_opts(opts);

    srv = (zcs_srt_server *)calloc(1, sizeof(*srv));
    if (!srv) {
        return NULL;
    }
    srv->poller = opts->poller;
    srv->port = opts->port ? opts->port : 9000;
    srv->epoll_id = -1;
    srv->listen_sock = srt_create_listener(opts->host, srv->port);
    if (srv->listen_sock == SRT_INVALID_SOCK) {
        free(srv);
        return NULL;
    }
    return srv;
}

zn_err_t zcs_srt_server_start(zcs_srt_server *srv)
{
    if (!srv || srv->running) {
        return ZN_ERR_INVALID;
    }
    srv->running = 1;
    zcs_srt_poller_init(srv);
    if (srv->epoll_id < 0 || !srv->io_timer) {
        srv->running = 0;
        return ZN_ERR_STATE;
    }
    zn_info("SRT listen 0.0.0.0:%u (poller epoll)", (unsigned)srv->port);
    return ZN_OK;
}

void zcs_srt_server_stop(zcs_srt_server *srv)
{
    if (!srv || !srv->running) {
        return;
    }
    srv->running = 0;
    zcs_srt_poller_fini(srv);
    if (srv->listen_sock != SRT_INVALID_SOCK) {
        srt_close(srv->listen_sock);
        srv->listen_sock = SRT_INVALID_SOCK;
    }
}

void zcs_srt_server_destroy(zcs_srt_server *srv)
{
    if (!srv) {
        return;
    }
    zcs_srt_server_stop(srv);
    free(srv);
    if (g_srt_service_initialized) {
        srt_cleanup();
        g_srt_service_initialized = 0;
    }
}

uint16_t zcs_srt_server_port(const zcs_srt_server *srv)
{
    return srv ? srv->port : 0;
}
