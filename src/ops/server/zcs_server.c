/**
 * @file zcs_server.c
 * @brief 媒体服务器接口：配置 / poller / 协议装配（自 zcs 上收）。
 */
#include "zcs/server.h"

#include "zcs/core/auth.h"
#include "zcs/util/dns_cache.h"
#include "zcs/core/module_registry.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/stream/url.h"
#include "zcs/egress/pacing/egress_pacing.h"
#include "zcs/publish/ingest/common/protocol_opts.h"
#include "zcs/pull/proxy.h"
#include "zcs/ops/api/webhook/webhook_client.h"
#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include "zcs/egress/recorder/egress_zs_recorder.h"
#include "zcs/ops/config/pull_ssl.h"
#include "zcs/session/http/http_server.h"
#include "zcs/session/rtmp/rtmp_server.h"
#include "zcs/session/rtsp/rtsp_server.h"
#include "zcs/session/rtsp/session_auth.h"
#include "zcs/protocol/rtsp/transport.h"
#include "zcs/util/buf_pool.h"
#include "zcs/util/poller_drain.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/gop/limits.h"
#include "zcs/ops/api/webapi/webapi.h"
#include "zcs/vod/io/source.h"
#include "zcs/util/thread_pool.h"
#include "zn/poller/poller_pool.h"
#include "zn/util/log.h"
#include "zn/net/ssl.h"
#include "zn/zn.h"

/** destroy 时排空 poller async 的等待（ms）；过短易漏掉跨线程 buf release */
#ifndef ZCS_SERVER_SHUTDOWN_DRAIN_MS
#define ZCS_SERVER_SHUTDOWN_DRAIN_MS 200u
#endif


#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

struct zcs_server {
    zcs_config cfg;
    zn_poller_pool *poller_pool;
    zn_poller *main_poller;
    unsigned poller_count;

    zcs_server_hooks hooks;
    int hooks_set;

    zcs_rtmp_server *rtmp;
    zcs_http_server *http;
    zn_ssl_ctx *ssl_ctx;
    zcs_rtsp_server *rtsp;
    zcs_live_pull_proxy *proxy;

    int started;
    volatile int stop_flag;
};

static zcs_server *g_signal_server;

/* 信号处理只置 stop_flag（异步信号安全：仅写 volatile int），真正的关闭
 * 由 zcs_server_run 的主循环轮询到标志后走正常销毁路径——绝不能在信号处理
 * 函数里做 malloc/free/加锁等非异步信号安全的操作。 */
#if defined(_WIN32)
static BOOL WINAPI zcs_server_win_ctrl(DWORD ctrl_type)
{
    if (ctrl_type == CTRL_C_EVENT || ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT || ctrl_type == CTRL_SHUTDOWN_EVENT) {
        if (g_signal_server) {
            g_signal_server->stop_flag = 1;
        }
        return TRUE;
    }
    return FALSE;
}
#else
static void zcs_server_sig_stop(int sig)
{
    (void)sig;
    if (g_signal_server) {
        g_signal_server->stop_flag = 1;
    }
}

static void zcs_server_sig_hup(int sig)
{
    (void)sig;
    zn_log_reopen_file();
}
#endif

static zn_err_t server_setup_logging(zcs_config *cfg, const char *log_override)
{
    if (log_override && log_override[0]) {
        strncpy(cfg->general.log_file, log_override, sizeof(cfg->general.log_file) - 1);
        cfg->general.log_file[sizeof(cfg->general.log_file) - 1] = '\0';
    }

    zn_log_set_level(cfg->general.log_level);
    if (cfg->general.log_file[0]) {
        if (zn_log_open_file(cfg->general.log_file) != 0) {
            zn_warn("failed to open log file: %s (console only)", cfg->general.log_file);
            return ZN_OK;
        }
        if (cfg->general.log_max_mb > 0) {
            zn_log_set_rotate((size_t)cfg->general.log_max_mb * 1024u * 1024u,
                               cfg->general.log_keep_count);
            zn_info("logging to stderr and %s (rotate every %u MB, keep %d)",
                     cfg->general.log_file, cfg->general.log_max_mb, cfg->general.log_keep_count);
        } else {
            zn_info("logging to stderr and %s (no rotate)", cfg->general.log_file);
        }
    }
    return ZN_OK;
}

/*
 * 运行时启动（不监听端口，只把全局子系统装配好）。顺序有依赖：
 *   1. 先应用各模块配置（pacing/protocol/vod/record/buf_pool）；
 *   2. gop 队列默认参数（target_gops/cache/bytes）必须在任何源建立前设置，
 *      否则早建立的队列会用到旧默认值；
 *   3. 平台/模块注册；
 *   4. 日志；
 *   5. 最后才起 poller 池——poller 一起来，定时器/回调就可能引用上面这些
 *      子系统，必须全部就绪。
 * vod 工作线程数 0 时强制成 1：否则所有异步 IO 会退化成在 poller 上同步做。
 */
static zn_err_t server_runtime_boot(zcs_server *s, const char *log_override)
{
    zn_poller_pool_opts_t popts;
    unsigned gop_tg;

    zcs_egress_pacing_init(&s->cfg.play.pacing);
    zcs_protocol_opts_set_defaults(&s->cfg.protocol);
    zcs_vhost_set_enabled(s->cfg.general.enable_vhost);
    zcs_vod_config_apply(&s->cfg);
    zcs_mp4_recorder_configure(&s->cfg.record);
    zcs_zs_recorder_configure(&s->cfg.storage);
    zcs_buf_pool_init(&s->cfg);
    /* 0 会让所有异步 IO 退化到 poller 线程上同步执行：强制至少 1 个线程。 */
    zcs_vod_thread_pool_init(s->cfg.general.vod_work_threads > 0 ? s->cfg.general.vod_work_threads
                                                                 : 1);
    gop_tg = s->cfg.general.gop_target_gops;
    if (gop_tg == 0) {
        gop_tg = ZCS_GOP_QUEUE_TARGET_GOPS;
    }
    zcs_gop_queue_set_default_target_gops(gop_tg);
    zcs_gop_queue_set_default_cache_ms(s->cfg.general.gop_cache_sec * 1000u);
    if (s->cfg.general.gop_max_bytes > 0) {
        zcs_gop_queue_set_default_byte_budget((size_t)s->cfg.general.gop_max_bytes);
    }

    zn_platform_init();
    zcs_modules_register_all();

    if (server_setup_logging(&s->cfg, log_override) != ZN_OK) {
        return ZN_ERR_IO;
    }

    s->poller_count = s->cfg.general.poller_threads > 0 ? s->cfg.general.poller_threads : 2;
    popts = (zn_poller_pool_opts_t){
        .size = s->poller_count,
        .prefer_current_thread = 0,
        .thread_priority = ZB_THREAD_PRIO_NORMAL,
    };
    s->poller_pool = zn_poller_pool_create(&popts);
    if (!s->poller_pool || zn_poller_pool_start(s->poller_pool) != ZN_OK) {
        zn_error("poller_pool start failed");
        if (s->poller_pool) {
            zn_poller_pool_destroy(s->poller_pool);
            s->poller_pool = NULL;
        }
        zcs_vod_thread_pool_fini();
        zcs_buf_pool_fini();
        return ZN_ERR_IO;
    }

    s->main_poller = zn_poller_pool_get(s->poller_pool, 0);
    zcs_poller_pool_buf_attach(s->poller_pool, &s->cfg);
    return ZN_OK;
}

static void server_runtime_shutdown(zcs_server *s)
{
    if (s->poller_pool) {
        /*
         * 1) 线程仍在跑时排空 async（含无锁池跨线程 release 回投）
         * 2) join poller 线程
         * 3) 再排空一次（stop 后队列里可能还有尾包）
         * 4) 再 detach 本地池，避免 owner 已空仍入 freelist
         */
        zcs_poller_pool_drain_async_wait(s->poller_pool, ZCS_SERVER_SHUTDOWN_DRAIN_MS);
        zn_poller_pool_stop(s->poller_pool);
        zcs_poller_pool_drain_async(s->poller_pool);
        zcs_poller_pool_buf_detach(s->poller_pool);
        zn_poller_pool_destroy(s->poller_pool);
        s->poller_pool = NULL;
    }
    s->main_poller = NULL;
    zcs_vod_thread_pool_fini();
    zcs_buf_pool_fini();
    zcs_egress_pacing_fini();
    zn_log_close_file();
    zn_platform_fini();
}

/* 汇总实际监听端口。服务已起时以服务实际绑定端口为准（可配置为 0 让内核
 * 分配，此时配置里的端口不可信），未起时回落到配置值。 */
static void server_fill_ports(const zcs_server *s, zcs_media_server_ports *ports)
{
    memset(ports, 0, sizeof(*ports));
    ports->rtmp = s->rtmp ? (unsigned)zcs_rtmp_server_port(s->rtmp) : s->cfg.rtmp.port;
    ports->rtsp = s->rtsp ? (unsigned)zcs_rtsp_server_port(s->rtsp) : s->cfg.rtsp.port;
    ports->http = s->http ? (unsigned)zcs_http_server_port(s->http) : s->cfg.http.port;
}

static void print_play_urls(const zcs_media_server_ports *ports, const zcs_media_source *src)
{
    zcs_media_urls u;

    if (!ports || !src) {
        return;
    }
    zcs_media_urls_build(&u, src, ports);
    if (u.rtsp_play[0]) {
        printf("      RTSP     : %s\r\n", u.rtsp_play);
    }
    if (u.rtmp_play[0]) {
        printf("      RTMP     : %s\r\n", u.rtmp_play);
    }
}

static void print_generic_play_urls(const zcs_media_server_ports *ports, const zcs_config *cfg)
{
    (void)cfg;
    printf("      RTSP     : rtsp://127.0.0.1:%u/live/{stream}\r\n", ports->rtsp);
    printf("      RTMP     : rtmp://127.0.0.1:%u/live/{stream}\r\n", ports->rtmp);
}

/**
 * 大并发（千路级推/拉）下每连接至少 1 个 fd。
 * 启动时把软限制抬到硬限制，失败只告警不阻断。
 */
static void raise_fd_limit(void)
{
#if !defined(_WIN32)
    struct rlimit rl;

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return;
    }
    if (rl.rlim_cur < rl.rlim_max) {
        rlim_t want = rl.rlim_max;
        rl.rlim_cur = want;
        if (setrlimit(RLIMIT_NOFILE, &rl) == 0) {
            zn_info("server: fd limit raised to %llu", (unsigned long long)want);
        } else {
            zn_warn("server: fd limit raise failed (cur=%llu), connections may hit the limit",
                     (unsigned long long)rl.rlim_cur);
        }
    } else {
        zn_info("server: fd limit cur=%llu max=%llu", (unsigned long long)rl.rlim_cur,
                 (unsigned long long)rl.rlim_max);
    }
#endif
}

/*
 * 启动所有监听服务。装配顺序体现依赖关系：
 *   1. 先初始化 webhook/auth 等“被动服务”（协议回调会引用它们）；
 *   2. 建 TLS 上下文（HTTP/RTMP/RTSP 共享）；
 *   3. 依次 create+start RTMP/HTTP/RTSP。
 * 任何一个监听启动失败都返回错误，由调用方 server_stop_services 整体回滚。
 */
static zn_err_t server_start_services(zcs_server *s)
{
    zcs_config *cfg = &s->cfg;

    /* 大并发下 fd 是首要资源，尽量在监听前就抬高。 */
    raise_fd_limit();
    zcs_media_events events;
    zcs_rtmp_server_opts ropts;
    zcs_http_server_opts fopts;
    zcs_rtsp_server_opts sopts;
    zcs_auth_options auth_opt;
    zn_err_t err;
    const char *lhost;

    memset(&events, 0, sizeof(events));
    if (s->hooks_set) {
        events = s->hooks;
    }

    memset(&auth_opt, 0, sizeof(auth_opt));
    auth_opt.enable = cfg->auth.enable;
    strncpy(auth_opt.token_param, cfg->auth.token_param, sizeof(auth_opt.token_param) - 1);

    zcs_media_events_set(s->main_poller, &events,
                         cfg->general.stream_none_reader_delay_ms);
    zcs_media_events_set_continue_push_ms(cfg->protocol.continue_push_ms);
    /* 预解析配置里的固定远端地址：避免首次连接时在 poller 上做 DNS。 */
    if (cfg->proxy.pull[0]) {
        zcs_dns_cache_warm(cfg->proxy.pull);
    }

    zcs_auth_init(&auth_opt);
    zcs_webhook_init(s->main_poller, cfg);

    lhost = cfg->general.listen_host[0] ? cfg->general.listen_host : "0.0.0.0";

    if (cfg->ssl.cert_file[0] && cfg->ssl.key_file[0]) {
        zn_ssl_config_t sslcfg;
        memset(&sslcfg, 0, sizeof(sslcfg));
        sslcfg.server_mode = 1;
        sslcfg.cert_file = cfg->ssl.cert_file;
        sslcfg.key_file = cfg->ssl.key_file;
        s->ssl_ctx = zn_ssl_ctx_create(&sslcfg);
        if (!s->ssl_ctx) {
            zn_warn("server TLS cert load failed: %s", cfg->ssl.cert_file);
        }
    }

    if (cfg->protocol.enable_rtmp) {
        memset(&ropts, 0, sizeof(ropts));
        ropts.poller_pool = s->poller_pool;
        ropts.host = lhost;
        ropts.port = (uint16_t)cfg->rtmp.port;
        ropts.ssl_ctx = s->ssl_ctx;
        ropts.ssl_port = (uint16_t)cfg->rtmp.ssl_port;
        ropts.handshake_timeout_ms = (uint64_t)cfg->protocol.handshake_timeout_sec * 1000u;
        ropts.idle_timeout_ms = (uint64_t)cfg->protocol.idle_timeout_sec * 1000u;
        ropts.max_sessions = (unsigned)cfg->protocol.max_sessions;
        ropts.max_sessions_per_ip = (unsigned)cfg->protocol.max_sessions_per_ip;
        s->rtmp = zcs_rtmp_server_create(&ropts);
        if (!s->rtmp) {
            zn_error("RTMP service create failed");
            return ZN_ERR_IO;
        }
    }

    memset(&fopts, 0, sizeof(fopts));
    fopts.poller = s->main_poller;
    fopts.poller_pool = s->poller_pool;
    fopts.host = lhost;
    fopts.port = (uint16_t)cfg->http.port;
    fopts.api_secret = cfg->http.api_secret;
    fopts.cfg = cfg;
    fopts.ssl_ctx = s->ssl_ctx;
    fopts.ssl_port = (uint16_t)cfg->http.ssl_port;
    fopts.handshake_timeout_ms = (uint64_t)cfg->protocol.handshake_timeout_sec * 1000u;
    fopts.idle_timeout_ms = (uint64_t)cfg->protocol.idle_timeout_sec * 1000u;
    fopts.max_sessions = (unsigned)cfg->protocol.max_sessions;
    fopts.max_sessions_per_ip = (unsigned)cfg->protocol.max_sessions_per_ip;
    s->http = zcs_http_server_create(&fopts);
    if (!s->http) {
        zn_error("HTTP service create failed");
        return ZN_ERR_IO;
    }

    if (cfg->protocol.enable_rtsp) {
        memset(&sopts, 0, sizeof(sopts));
        sopts.poller_pool = s->poller_pool;
        sopts.host = lhost;
        sopts.port = (uint16_t)cfg->rtsp.port;
        sopts.advertise_host = cfg->general.extern_ip[0] ? cfg->general.extern_ip : NULL;
        sopts.ssl_ctx = s->ssl_ctx;
        sopts.ssl_port = (uint16_t)cfg->rtsp.ssl_port;
        sopts.handshake_timeout_ms = (uint64_t)cfg->protocol.handshake_timeout_sec * 1000u;
        sopts.idle_timeout_ms = (uint64_t)cfg->protocol.idle_timeout_sec * 1000u;
        sopts.max_sessions = (unsigned)cfg->protocol.max_sessions;
        sopts.max_sessions_per_ip = (unsigned)cfg->protocol.max_sessions_per_ip;
        s->rtsp = zcs_rtsp_server_create(&sopts);
        if (!s->rtsp) {
            zn_error("RTSP service create failed");
            return ZN_ERR_IO;
        }
    }


    if (s->rtmp) {
        err = zcs_rtmp_server_start(s->rtmp);
        if (err != ZN_OK) {
            zn_error("RTMP listen failed port=%u: %s", cfg->rtmp.port, zn_strerror(err));
            return err;
        }
    }

    err = zcs_http_server_start(s->http);
    if (err != ZN_OK) {
        zn_error("HTTP listen failed port=%u: %s", cfg->http.port, zn_strerror(err));
        return err;
    }

    if (s->rtsp) {
        if (cfg->rtsp.auth_user[0]) {
            zcs_rtsp_auth_configure(cfg->rtsp.auth_user, cfg->rtsp.auth_pass);
            zn_info("RTSP Digest auth enabled user=%s", cfg->rtsp.auth_user);
        }
        zcs_rtsp_set_test_reject_tcp_setup(cfg->rtsp.test_reject_tcp_setup);
        zcs_rtsp_set_rtp_transport_type(cfg->rtsp.rtp_transport_type);
        zcs_rtsp_multicast_configure(cfg->rtsp.multicast_addr, cfg->rtsp.multicast_port_base,
                                     cfg->rtsp.multicast_ttl);
        zcs_rtsp_set_low_latency(cfg->rtsp.low_latency);

        err = zcs_rtsp_server_start(s->rtsp);
        if (err != ZN_OK) {
            zn_error("RTSP listen failed port=%u: %s", cfg->rtsp.port, zn_strerror(err));
#if !defined(_WIN32)
            if (cfg->rtsp.port < 1024) {
                zn_error("RTSP port %u needs root on Linux; use e.g. 8554 in config.ini",
                          cfg->rtsp.port);
            }
#endif
            return err;
        }
    }


    /* 所有监听已就绪、端口确定，向 web 平台自注册并开启心跳续注册。 */
    {
        zcs_media_server_ports ports;

        server_fill_ports(s, &ports);
        zcs_webhook_server_started(cfg, &ports);
        zcs_webhook_start_keepalive(cfg, &ports);
        /* 云存储节点独立自注册与心跳（与取流节点分离，供平台登记独立存储节点）。 */
        zcs_webhook_storage_started(cfg, &ports);
        zcs_webhook_start_storage_keepalive(cfg, &ports);
    }

    return ZN_OK;
}

/*
 * 拉流代理：把配置里的远端 url 拉进来当本地源供播放（ZLM 的 pull 语义）。
 * 单条、可选配置；起点失败即整体失败返回 IO 错误（代理通常是部署的核心，
 * 静默失败会导致“配置了却播不了”难排查）。create 成功后 start 失败要销毁
 * 已创建对象，避免半初始化泄漏。
 */
static zn_err_t server_start_proxy(zcs_server *s)
{
    zcs_config *cfg = &s->cfg;

    if (!cfg->proxy.pull[0]) {
        return ZN_OK;
    }

    {
        zcs_live_pull_proxy_opts popt = {
            .poller_pool = s->poller_pool,
            .pull_url = cfg->proxy.pull,
            .ssl_ctx = zcs_pull_ssl_ctx(cfg),
            .app = cfg->proxy.app[0] ? cfg->proxy.app : NULL,
            .stream = cfg->proxy.stream[0] ? cfg->proxy.stream : NULL,
            .proxy_prefix = cfg->proxy.prefix[0] ? cfg->proxy.prefix : NULL,
            .retry_count = cfg->proxy.retry_count,
            .reconnect_delay_ms = cfg->proxy.reconnect_delay_ms,
        };
        s->proxy = zcs_live_pull_proxy_create(&popt);
        if (!s->proxy || zcs_live_pull_proxy_start(s->proxy) != ZN_OK) {
            zn_error("live_pull_proxy start failed: %s", cfg->proxy.pull);
            zcs_live_pull_proxy_destroy(s->proxy);
            s->proxy = NULL;
            return ZN_ERR_IO;
        }
        zn_info("proxy active: %s -> %s/%s", cfg->proxy.pull, zcs_live_pull_proxy_app(s->proxy),
                 zcs_live_pull_proxy_stream(s->proxy));
    }
    return ZN_OK;
}

static void server_stop_services(zcs_server *s)
{
    /* 关闭顺序与启动大致相反：先停 webhook/auth 等中间层，最后才停底层监听
     * （RTSP/HTTP/RTMP）与事件系统。 */
    zcs_webhook_fini();
    zcs_auth_fini();
    zcs_live_pull_proxy_destroy(s->proxy);
    s->proxy = NULL;
    if (s->rtsp) {
        zcs_rtsp_server_stop(s->rtsp);
        zcs_rtsp_server_destroy(s->rtsp);
        s->rtsp = NULL;
    }
    zcs_rtsp_udp_registry_fini();
    if (s->http) {
        zcs_http_server_stop(s->http);
        zcs_http_server_destroy(s->http);
        s->http = NULL;
    }
    if (s->rtmp) {
        zcs_rtmp_server_stop(s->rtmp);
        zcs_rtmp_server_destroy(s->rtmp);
        s->rtmp = NULL;
    }
    zcs_media_events_fini();
    if (s->ssl_ctx) {
        zn_ssl_ctx_destroy(s->ssl_ctx);
        s->ssl_ctx = NULL;
    }
    s->started = 0;
}

static zcs_server *server_alloc(void)
{
    zcs_server *s = (zcs_server *)calloc(1, sizeof(*s));
    return s;
}

zcs_server *zcs_server_create_from_ini(const char *config_path, const char *log_override)
{
    zcs_server *s = server_alloc();
    if (!s) {
        return NULL;
    }

    /* 先取默认配置再覆盖 ini：保证 ini 里缺省的项都有合理默认值。
     * ini 解析失败是硬错误（用户显式给了路径却读不了，不能静默用默认）。 */
    zcs_config_default(&s->cfg);
    if (config_path && config_path[0]) {
        if (zcs_config_load_ini(&s->cfg, config_path) != ZN_OK) {
            fprintf(stderr, "failed to load config: %s\r\n", config_path);
            free(s);
            return NULL;
        }
    }

    if (server_runtime_boot(s, log_override) != ZN_OK) {
        free(s);
        return NULL;
    }
    return s;
}

/* 用调用方提供的完整配置创建 server。cfg 为 NULL 时用默认配置。
 * 与 create_from_ini 共用 alloc+runtime_boot，区别只在配置来源。 */
zcs_server *zcs_server_create(const zcs_config *cfg)
{
    zcs_server *s = server_alloc();
    if (!s) {
        return NULL;
    }
    if (cfg) {
        s->cfg = *cfg;
    } else {
        zcs_config_default(&s->cfg);
    }
    if (server_runtime_boot(s, NULL) != ZN_OK) {
        free(s);
        return NULL;
    }
    return s;
}

void zcs_server_destroy(zcs_server *s)
{
    if (!s) {
        return;
    }
    /* 先解除信号钩子指向本对象，防止销毁后信号再写已释放内存。 */
    if (g_signal_server == s) {
        g_signal_server = NULL;
    }
    /* 只有真正 start 过才需要 stop services；但 runtime（poller 池等）无论
     * 起没起 services 都在 boot 中建过，必须无条件 shutdown。 */
    if (s->started) {
        server_stop_services(s);
    }
    server_runtime_shutdown(s);
    free(s);
}

/* hooks 必须在 start 之前设置：start_services 把它们拷进 zcs_media_events 后
 * 就固定了，start 之后再改不会生效（只影响下次 start）。 */
int zcs_server_set_hooks(zcs_server *s, const zcs_server_hooks *hooks)
{
    if (!s) {
        return -1;
    }
    if (hooks) {
        s->hooks = *hooks;
        s->hooks_set = 1;
    } else {
        memset(&s->hooks, 0, sizeof(s->hooks));
        s->hooks_set = 0;
    }
    return 0;
}

zn_err_t zcs_server_start(zcs_server *s)
{
    zn_err_t err;

    if (!s) {
        return ZN_ERR_INVALID;
    }
    if (s->started) {
        return ZN_OK;
    }

    err = server_start_services(s);
    if (err != ZN_OK) {
        /* 启动到一半失败也要回滚已起的服务，避免半开状态泄漏端口/线程。 */
        server_stop_services(s);
        return err;
    }

    /* 代理拉流放在监听服务之后：它依赖 HTTP/RTMP 已就绪才能把拉到的流挂上去。
     * 代理失败不致命（不阻断主服务），仅记日志。 */
    (void)server_start_proxy(s);
    s->started = 1;
    /* 清零 stop_flag：允许一个 server 实例被 stop 后再 start。 */
    s->stop_flag = 0;

    {
        zcs_media_server_ports ports;
        server_fill_ports(s, &ports);
        zcs_web_api_note_boot();
        zn_info("Zero Cloud Storage (ZCS) started (poller_pool=%u)", s->poller_count);
#ifdef _WIN32
        zn_info("process pid=%lu", (unsigned long)GetCurrentProcessId());
#endif
        zn_info("Listen RTMP=%u RTSP=%u HTTP=%u", ports.rtmp, ports.rtsp, ports.http);
        zcs_media_events_set_server_ports(&ports);
        zcs_media_source_log_registry(&ports);
    }
    return ZN_OK;
}

void zcs_server_stop(zcs_server *s)
{
    if (!s) {
        return;
    }
    s->stop_flag = 1;
    if (s->started) {
        zn_info("shutting down...");
        server_stop_services(s);
    }
}

/* 只请求停止（置标志），由 zcs_server_run 的循环感知并优雅关闭；与
 * zcs_server_stop 的区别是后者会立即同步停掉服务。给外部（如另一个线程）
 * 一个线程安全的“请停”入口。 */
void zcs_server_request_stop(zcs_server *s)
{
    if (s) {
        s->stop_flag = 1;
    }
}

/* 注册进程信号。SIGTERM/SIGINT 请求停止；SIGHUP 重开日志文件（logrotate 的
 * 常用约定：切分后发 HUP 让进程重开句柄）；SIGPIPE 忽略（见下方）。 */
void zcs_server_install_default_signals(zcs_server *s)
{
    g_signal_server = s;
#if defined(_WIN32)
    SetConsoleCtrlHandler(zcs_server_win_ctrl, TRUE);
#else
    {
        struct sigaction sa;
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = zcs_server_sig_stop;
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
        sa.sa_handler = zcs_server_sig_hup;
        sigaction(SIGHUP, &sa, NULL);
        /* 忽略 SIGPIPE：向已关闭的 socket 写会触发它，默认行为是杀进程。
         * 媒体服务器大量发生“对端先断”，必须靠 write 返回 EPIPE 自行处理。 */
        signal(SIGPIPE, SIG_IGN);
    }
#endif
}

void zcs_server_run(zcs_server *s, int seconds)
{
    if (!s) {
        return;
    }
    /* seconds < 0：常驻运行直到收到停止信号（生产模式）。否则跑固定时长
     * （seconds=0 视作 5 秒，测试/演练用）。轮询 200ms 兼顾响应性与 CPU。 */
    if (seconds < 0) {
        while (!s->stop_flag) {
            zn_sleep_ms(200);
        }
        zn_info("signal received, shutting down...");
    } else {
        int run = seconds > 0 ? seconds : 5;
        int elapsed = 0;
        printf("running for %d second(s)\r\n", run);
        while (!s->stop_flag && elapsed < run * 5) {
            zn_sleep_ms(200);
            ++elapsed;
        }
    }
}

const zcs_config *zcs_server_config(const zcs_server *s)
{
    return s ? &s->cfg : NULL;
}

int zcs_server_is_running(const zcs_server *s)
{
    return s && s->started;
}

void zcs_server_print_endpoints(const zcs_server *s)
{
    zcs_media_server_ports ports;
    const zcs_config *cfg;

    if (!s || !s->started) {
        return;
    }
    cfg = &s->cfg;
    server_fill_ports(s, &ports);

    printf("Zero Cloud Storage (ZCS) running (log: %s)\r\n", cfg->general.log_file);
    if (s->proxy) {
        printf("  [proxy] %s -> %s/%s\r\n", cfg->proxy.pull, zcs_live_pull_proxy_app(s->proxy),
               zcs_live_pull_proxy_stream(s->proxy));
    } else {
        printf("  [1] Publish:\r\n");
        if (s->rtmp) {
            printf("      ffmpeg -re -stream_loop -1 -i {file}.mp4 -c copy -f flv "
                   "rtmp://127.0.0.1:%u/live/{stream}\r\n",
                   ports.rtmp);
        }
    }
    printf("  [2] Play:\r\n");
    if (s->proxy) {
        zcs_media_source *psrc = zcs_live_pull_proxy_source(s->proxy);
        if (psrc) {
            print_play_urls(&ports, psrc);
        } else {
            print_generic_play_urls(&ports, cfg);
        }
    } else {
        print_generic_play_urls(&ports, cfg);
    }
    printf("Press Ctrl+C to exit.\r\n");
}
