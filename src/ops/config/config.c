/**
 * @file config.c
 * @brief config.ini 默认值与加载。
 */
#include "zcs/ops/config/config.h"
#include "zcs/egress/pacing/egress_pacing.h"
#include "zcs/core/gop/limits.h"
#include "zn/util/log.h"
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 字符串辅助 ───────────────────────────────────────────────────────── */

static void trim(char *s)
{
    if (!s) {
        return;
    }
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static int parse_bool(const char *v)
{
    if (!v || !v[0]) {
        return 0;
    }
    if (v[0] == '0' || v[0] == 'f' || v[0] == 'F' || v[0] == 'n' || v[0] == 'N') {
        return 0;
    }
    return 1;
}

static int parse_log_level(const char *v)
{
    if (!v) {
        return ZN_LOG_INFO;
    }
    if (strcmp(v, "trace") == 0 || strcmp(v, "0") == 0) {
        return ZN_LOG_TRACE;
    }
    if (strcmp(v, "debug") == 0 || strcmp(v, "1") == 0) {
        return ZN_LOG_DEBUG;
    }
    if (strcmp(v, "warn") == 0 || strcmp(v, "2") == 0) {
        return ZN_LOG_WARN;
    }
    if (strcmp(v, "error") == 0 || strcmp(v, "3") == 0) {
        return ZN_LOG_ERROR;
    }
    return ZN_LOG_INFO;
}

/* ── 表驱动字段描述 ───────────────────────────────────────────────────────── */

typedef enum {
    ZCS_CFG_F_STR,   /* strncpy */
    ZCS_CFG_F_INT,   /* atoi */
    ZCS_CFG_F_UINT,  /* (unsigned)atoi */
    ZCS_CFG_F_FLOAT, /* (float)atof */
    ZCS_CFG_F_BOOL,  /* parse_bool */
    ZCS_CFG_F_CUSTOM /* 自定义解析函数 */
} zcs_cfg_ftype;

typedef struct zcs_cfg_field {
    const char *key;
    const char *alias; /* 旧驼峰别名，NULL 表示无别名 */
    zcs_cfg_ftype type;
    size_t offset;   /* offsetof(zcs_config, section.field) */
    size_t str_size; /* type==STR 时为字符串缓冲区大小 */
    void (*custom_fn)(zcs_config *, const char *);
} zcs_cfg_field;

/* 写入辅助 */
static void field_apply(zcs_config *cfg, const zcs_cfg_field *f, const char *val)
{
    void *ptr = (char *)cfg + f->offset;
    switch (f->type) {
    case ZCS_CFG_F_STR:
        strncpy((char *)ptr, val, f->str_size - 1);
        ((char *)ptr)[f->str_size - 1] = '\0';
        trim((char *)ptr);
        break;
    case ZCS_CFG_F_INT:
        *(int *)ptr = atoi(val);
        break;
    case ZCS_CFG_F_UINT:
        *(unsigned *)ptr = (unsigned)atoi(val);
        break;
    case ZCS_CFG_F_FLOAT:
        *(float *)ptr = (float)atof(val);
        break;
    case ZCS_CFG_F_BOOL:
        *(int *)ptr = parse_bool(val);
        break;
    case ZCS_CFG_F_CUSTOM:
        if (f->custom_fn) {
            f->custom_fn(cfg, val);
        }
        break;
    }
}

/* CUSTOM 解析函数 */
static void parse_log_level_fn(zcs_config *cfg, const char *val)
{
    cfg->general.log_level = parse_log_level(val);
}

static void parse_buf_pool_mode_fn(zcs_config *cfg, const char *val)
{
    if (strcmp(val, "global") == 0) {
        cfg->general.buf_pool_mode = ZCS_BUF_POOL_MODE_GLOBAL;
    } else if (strcmp(val, "per_poller") == 0 || strcmp(val, "perPoller") == 0) {
        cfg->general.buf_pool_mode = ZCS_BUF_POOL_MODE_PER_POLLER;
    } else {
        cfg->general.buf_pool_mode = ZCS_BUF_POOL_MODE_HYBRID;
    }
}

/* ── section 字段表 ────────────────────────────────────────────────────── */

#define OFF(section, field) offsetof(zcs_config, section.field)
#define STSZ(section, field) sizeof(((zcs_config *)0)->section.field)

/* [general] */
static const zcs_cfg_field k_general[] = {
    {"log_file", "logFile", ZCS_CFG_F_STR, OFF(general, log_file), STSZ(general, log_file), NULL},
    {"log_level", "logLevel", ZCS_CFG_F_CUSTOM, 0, 0, parse_log_level_fn},
    {"log_max_mb", "logMaxMB", ZCS_CFG_F_UINT, OFF(general, log_max_mb), 0, NULL},
    {"log_keep_count", "logKeepCount", ZCS_CFG_F_INT, OFF(general, log_keep_count), 0, NULL},
    {"poller_threads", "pollerThreads", ZCS_CFG_F_UINT, OFF(general, poller_threads), 0, NULL},
    {"stream_none_reader_delay_ms", "streamNoneReaderDelayMS", ZCS_CFG_F_INT,
     OFF(general, stream_none_reader_delay_ms), 0, NULL},
    {"buf_pool_enable", "bufPoolEnable", ZCS_CFG_F_BOOL, OFF(general, buf_pool_enable), 0, NULL},
    {"buf_pool_mode", "bufPoolMode", ZCS_CFG_F_CUSTOM, 0, 0, parse_buf_pool_mode_fn},
    {"buf_pool_max", "bufPoolMaxPerBucket", ZCS_CFG_F_UINT, OFF(general, buf_pool_max_per_bucket),
     0, NULL},
    {"gop_target_gops", "gopTargetGops", ZCS_CFG_F_UINT, OFF(general, gop_target_gops), 0, NULL},
    {"gop_cache_sec", "gopCacheSec", ZCS_CFG_F_UINT, OFF(general, gop_cache_sec), 0, NULL},
    {"gop_max_bytes", "gopMaxBytes", ZCS_CFG_F_UINT, OFF(general, gop_max_bytes), 0, NULL},
    {"vod_work_threads", "vodWorkThreads", ZCS_CFG_F_UINT, OFF(general, vod_work_threads), 0, NULL},
    {"extern_ip", "externIP", ZCS_CFG_F_STR, OFF(general, extern_ip), STSZ(general, extern_ip),
     NULL},
    {"listen_host", "listenHost", ZCS_CFG_F_STR, OFF(general, listen_host),
     STSZ(general, listen_host), NULL},
    {"enableVhost", "enable_vhost", ZCS_CFG_F_BOOL, OFF(general, enable_vhost), 0, NULL},
    /* media_server_id 属于 hook 但习惯放 general section */
    {"media_server_id", "mediaServerId", ZCS_CFG_F_STR, OFF(hook, media_server_id),
     STSZ(hook, media_server_id), NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [rtmp] */
static const zcs_cfg_field k_rtmp[] = {
    {"port", NULL, ZCS_CFG_F_UINT, OFF(rtmp, port), 0, NULL},
    {"sslport", NULL, ZCS_CFG_F_UINT, OFF(rtmp, ssl_port), 0, NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [rtsp] */
static const zcs_cfg_field k_rtsp[] = {
    {"port", NULL, ZCS_CFG_F_UINT, OFF(rtsp, port), 0, NULL},
    {"sslport", NULL, ZCS_CFG_F_UINT, OFF(rtsp, ssl_port), 0, NULL},
    {"auth_user", NULL, ZCS_CFG_F_STR, OFF(rtsp, auth_user), STSZ(rtsp, auth_user), NULL},
    {"auth_pass", NULL, ZCS_CFG_F_STR, OFF(rtsp, auth_pass), STSZ(rtsp, auth_pass), NULL},
    {"test_reject_tcp_setup", NULL, ZCS_CFG_F_BOOL, OFF(rtsp, test_reject_tcp_setup), 0, NULL},
    {"rtpTransportType", "rtp_transport_type", ZCS_CFG_F_INT, OFF(rtsp, rtp_transport_type), 0, NULL},
    {"multicast_addr", "multicastAddr", ZCS_CFG_F_STR, OFF(rtsp, multicast_addr),
     STSZ(rtsp, multicast_addr), NULL},
    {"multicast_port_base", "multicastPortBase", ZCS_CFG_F_UINT, OFF(rtsp, multicast_port_base), 0,
     NULL},
    {"multicast_ttl", "multicastTtl", ZCS_CFG_F_INT, OFF(rtsp, multicast_ttl), 0, NULL},
    {"low_latency", "lowLatency", ZCS_CFG_F_BOOL, OFF(rtsp, low_latency), 0, NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [http] */
static const zcs_cfg_field k_http[] = {
    {"port", NULL, ZCS_CFG_F_UINT, OFF(http, port), 0, NULL},
    {"sslport", NULL, ZCS_CFG_F_UINT, OFF(http, ssl_port), 0, NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [api] api.secret 逻辑上属于 http */
static const zcs_cfg_field k_api[] = {
    {"secret", NULL, ZCS_CFG_F_STR, OFF(http, api_secret), STSZ(http, api_secret), NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [srt] */

/* [webrtc] */

/* [protocol] */
static const zcs_cfg_field k_protocol[] = {
    {"enable_rtmp", NULL, ZCS_CFG_F_BOOL, OFF(protocol, enable_rtmp), 0, NULL},
    {"enable_rtsp", NULL, ZCS_CFG_F_BOOL, OFF(protocol, enable_rtsp), 0, NULL},
    {"enable_audio", NULL, ZCS_CFG_F_BOOL, OFF(protocol, enable_audio), 0, NULL},
    {"add_mute_audio", "addMuteAudio", ZCS_CFG_F_BOOL, OFF(protocol, add_mute_audio), 0, NULL},
    {"continue_push_ms", "continuePushMs", ZCS_CFG_F_INT, OFF(protocol, continue_push_ms), 0, NULL},
    {"modify_stamp", "modifyStamp", ZCS_CFG_F_INT, OFF(protocol, modify_stamp), 0, NULL},
    {"handshake_timeout_sec", "handshakeTimeoutSec", ZCS_CFG_F_INT,
     OFF(protocol, handshake_timeout_sec), 0, NULL},
    {"idle_timeout_sec", "idleTimeoutSec", ZCS_CFG_F_INT, OFF(protocol, idle_timeout_sec), 0, NULL},
    {"max_sessions", "maxSessions", ZCS_CFG_F_INT, OFF(protocol, max_sessions), 0, NULL},
    {"max_sessions_per_ip", "maxSessionsPerIp", ZCS_CFG_F_INT,
     OFF(protocol, max_sessions_per_ip), 0, NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [hls] */

/* [proxy] */
static const zcs_cfg_field k_proxy[] = {
    {"proxy_pull", "pull_url", ZCS_CFG_F_STR, OFF(proxy, pull), STSZ(proxy, pull), NULL},
    {"proxy_app", "app", ZCS_CFG_F_STR, OFF(proxy, app), STSZ(proxy, app), NULL},
    {"proxy_stream", "stream", ZCS_CFG_F_STR, OFF(proxy, stream), STSZ(proxy, stream), NULL},
    {"proxy_prefix", "prefix", ZCS_CFG_F_STR, OFF(proxy, prefix), STSZ(proxy, prefix), NULL},
    {"retry_count", NULL, ZCS_CFG_F_INT, OFF(proxy, retry_count), 0, NULL},
    {"reconnect_delay_ms", NULL, ZCS_CFG_F_INT, OFF(proxy, reconnect_delay_ms), 0, NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [record] */
static const zcs_cfg_field k_record[] = {
    {"app", "appName", ZCS_CFG_F_STR, OFF(record, app), STSZ(record, app), NULL},
    {"root", "mp4_save_path", ZCS_CFG_F_STR, OFF(record, root), STSZ(record, root), NULL},
    {"file_repeat", "fileRepeat", ZCS_CFG_F_BOOL, OFF(record, file_repeat), 0, NULL},
    {"mp4_max_second", "mp4MaxSecond", ZCS_CFG_F_INT, OFF(record, mp4_max_second), 0, NULL},
    {"max_total_mb", "maxTotalMb", ZCS_CFG_F_INT, OFF(record, max_total_mb), 0, NULL},
    {"free_space_check_mb", "freeSpaceCheckMb", ZCS_CFG_F_INT, OFF(record, free_space_check_mb), 0,
     NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [storage] 中心云存储 */
static const zcs_cfg_field k_storage[] = {
    {"enable", "storageEnable", ZCS_CFG_F_BOOL, OFF(storage, enable), 0, NULL},
    {"dir", "storageDir", ZCS_CFG_F_STR, OFF(storage, dir), STSZ(storage, dir), NULL},
    {"capacity_gb", "capacityGb", ZCS_CFG_F_INT, OFF(storage, capacity_gb), 0, NULL},
    {"retention_days", "retentionDays", ZCS_CFG_F_INT, OFF(storage, retention_days), 0, NULL},
    {"slot_size_mb", "slotSizeMb", ZCS_CFG_F_INT, OFF(storage, slot_size_mb), 0, NULL},
    {"index_kb", "indexKb", ZCS_CFG_F_INT, OFF(storage, index_kb), 0, NULL},
    {"free_watermark_pct", "freeWatermarkPct", ZCS_CFG_F_INT, OFF(storage, free_watermark_pct), 0,
     NULL},
    {"flush_kb", "flushKb", ZCS_CFG_F_INT, OFF(storage, flush_kb), 0, NULL},
    {"flush_ms", "flushMs", ZCS_CFG_F_INT, OFF(storage, flush_ms), 0, NULL},
    {"slot_max_second", "slotMaxSecond", ZCS_CFG_F_INT, OFF(storage, slot_max_second), 0, NULL},
    {"seal_margin_mb", "sealMarginMb", ZCS_CFG_F_INT, OFF(storage, seal_margin_mb), 0, NULL},
    {"pull_host", "pullHost", ZCS_CFG_F_STR, OFF(storage, pull_host), STSZ(storage, pull_host), NULL},
    {"pull_port", "pullPort", ZCS_CFG_F_INT, OFF(storage, pull_port), 0, NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [play] 所有字段属于 play.pacing */
#define POFF(field) offsetof(zcs_config, play.pacing.field)
static const zcs_cfg_field k_play[] = {
    {"ring_max_lag", "ringMaxLag", ZCS_CFG_F_UINT, POFF(ring_max_lag), 0, NULL},
    {"catchup_lag", "catchupLag", ZCS_CFG_F_UINT, POFF(catchup_lag), 0, NULL},
    {"frame_budget_live", "frameBudgetLive", ZCS_CFG_F_UINT, POFF(frame_budget_live), 0, NULL},
    {"frame_budget_catchup", "frameBudgetCatchup", ZCS_CFG_F_UINT, POFF(frame_budget_catchup), 0,
     NULL},
    {"burst_budget", "burstBudget", ZCS_CFG_F_UINT, POFF(burst_budget), 0, NULL},
    {"flv_budget_live", "flvBudgetLive", ZCS_CFG_F_UINT, POFF(flv_budget_live), 0, NULL},
    {"flv_budget_vod", "flvBudgetVod", ZCS_CFG_F_UINT, POFF(flv_budget_vod), 0, NULL},
    {"pace_lead_ms", "paceLeadMs", ZCS_CFG_F_UINT, POFF(pace_lead_ms), 0, NULL},
    {"resync_lag", "resyncLag", ZCS_CFG_F_UINT, POFF(resync_lag), 0, NULL},
    {"resync_lag_max", "resyncLagMax", ZCS_CFG_F_UINT, POFF(resync_lag_max), 0, NULL},
    {"resync_cooldown_ms", "resyncCooldownMs", ZCS_CFG_F_UINT, POFF(resync_cooldown_ms), 0, NULL},
    {"slow_consumer_kick_lag", "slowConsumerKickLag", ZCS_CFG_F_UINT, POFF(slow_consumer_kick_lag),
     0, NULL},
    {"vod_catchup_start", "vodCatchupStart", ZCS_CFG_F_UINT, POFF(vod_catchup_start), 0, NULL},
    {"vod_catchup_seek", "vodCatchupSeek", ZCS_CFG_F_UINT, POFF(vod_catchup_seek), 0, NULL},
    {"vod_prefill_lag", "vodPrefillLag", ZCS_CFG_F_UINT, POFF(vod_prefill_lag), 0, NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};
#undef POFF

/* [hook] */
#define HOFF(field) offsetof(zcs_config, hook.field)
#define HSSZ(field) sizeof(((zcs_config *)0)->hook.field)
static const zcs_cfg_field k_hook[] = {
    {"enable", NULL, ZCS_CFG_F_BOOL, HOFF(enable), 0, NULL},
    {"timeout_sec", "timeoutSec", ZCS_CFG_F_FLOAT, HOFF(timeout_sec), 0, NULL},
    {"retry", NULL, ZCS_CFG_F_INT, HOFF(retry_count), 0, NULL},
    {"retry_delay", NULL, ZCS_CFG_F_FLOAT, HOFF(retry_delay_sec), 0, NULL},
    {"on_stream_changed", NULL, ZCS_CFG_F_STR, HOFF(on_stream_changed), HSSZ(on_stream_changed),
     NULL},
    {"on_server_started", NULL, ZCS_CFG_F_STR, HOFF(on_server_started), HSSZ(on_server_started),
     NULL},
    {"on_server_keepalive", NULL, ZCS_CFG_F_STR, HOFF(on_server_keepalive),
     HSSZ(on_server_keepalive), NULL},
    {"keepalive_interval_sec", "keepaliveIntervalSec", ZCS_CFG_F_INT,
     HOFF(keepalive_interval_sec), 0, NULL},
    {"on_stream_none_reader", NULL, ZCS_CFG_F_STR, HOFF(on_stream_none_reader),
     HSSZ(on_stream_none_reader), NULL},
    {"on_play_stop", NULL, ZCS_CFG_F_STR, HOFF(on_play_stop), HSSZ(on_play_stop), NULL},
    {"on_record_mp4", NULL, ZCS_CFG_F_STR, HOFF(on_record_mp4), HSSZ(on_record_mp4), NULL},
    {"on_storage_started", NULL, ZCS_CFG_F_STR, HOFF(on_storage_started),
     HSSZ(on_storage_started), NULL},
    {"on_storage_keepalive", NULL, ZCS_CFG_F_STR, HOFF(on_storage_keepalive),
     HSSZ(on_storage_keepalive), NULL},
    {"on_record_ps", NULL, ZCS_CFG_F_STR, HOFF(on_record_ps), HSSZ(on_record_ps), NULL},
    {"stream_changed_schemas", NULL, ZCS_CFG_F_STR, HOFF(stream_changed_schemas),
     HSSZ(stream_changed_schemas), NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};
#undef HOFF
#undef HSSZ

/* [auth] */
#define AOFF(field) offsetof(zcs_config, auth.field)
#define ASSZ(field) sizeof(((zcs_config *)0)->auth.field)
static const zcs_cfg_field k_auth[] = {
    {"enable", NULL, ZCS_CFG_F_BOOL, AOFF(enable), 0, NULL},
    {"token_param", "tokenParam", ZCS_CFG_F_STR, AOFF(token_param), ASSZ(token_param), NULL},
    {"secret", NULL, ZCS_CFG_F_STR, AOFF(secret), ASSZ(secret), NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};
#undef AOFF
#undef ASSZ

/* [ssl] */
static const zcs_cfg_field k_ssl[] = {
    {"cert_file", "certFile", ZCS_CFG_F_STR, OFF(ssl, cert_file), STSZ(ssl, cert_file), NULL},
    {"key_file", "keyFile", ZCS_CFG_F_STR, OFF(ssl, key_file), STSZ(ssl, key_file), NULL},
    {"ca_file", "caFile", ZCS_CFG_F_STR, OFF(ssl, ca_file), STSZ(ssl, ca_file), NULL},
    {"verify_peer", "verifyPeer", ZCS_CFG_F_BOOL, OFF(ssl, verify_peer), 0, NULL},
    {"client_cert", NULL, ZCS_CFG_F_STR, OFF(ssl, client_cert), STSZ(ssl, client_cert), NULL},
    {"client_key", NULL, ZCS_CFG_F_STR, OFF(ssl, client_key), STSZ(ssl, client_key), NULL},
    {NULL, NULL, ZCS_CFG_F_STR, 0, 0, NULL}};

/* [onvif] */

/* [gb28181] */

/* [gb28181_platform] */

#undef OFF
#undef STSZ

/* ── section 字段表映射 ─────────────────────────────────────────────────── */

typedef struct {
    const char *section;
    const zcs_cfg_field *fields;
} zcs_section_map;

static const zcs_section_map k_sections[] = {
    {"general", k_general}, {"rtmp", k_rtmp},   {"rtsp", k_rtsp},     {"http", k_http},
    {"api", k_api},         {"protocol", k_protocol},
    {"proxy", k_proxy}, {"record", k_record}, {"play", k_play},
    {"hook", k_hook},       {"auth", k_auth},   {"ssl", k_ssl},
    {"storage", k_storage}, {NULL, NULL}};

/* ── apply_kv：查表写入 ───────────────────────────────────────────────────── */

/** 键值写入结果，用于在加载时提示拼写错误（否则配置静默失效，最难排查）。 */
typedef enum {
    ZCS_KV_OK = 0,
    ZCS_KV_UNKNOWN_SECTION,
    ZCS_KV_UNKNOWN_KEY,
} zcs_kv_result;

static zcs_kv_result apply_kv(zcs_config *cfg, const char *section, const char *key,
                              const char *val)
{
    if (!cfg || !section || !key || !val) {
        return ZCS_KV_UNKNOWN_SECTION;
    }
    for (const zcs_section_map *sm = k_sections; sm->section; ++sm) {
        if (strcmp(sm->section, section) != 0) {
            continue;
        }
        for (const zcs_cfg_field *f = sm->fields; f->key; ++f) {
            if (strcmp(f->key, key) == 0 || (f->alias && strcmp(f->alias, key) == 0)) {
                field_apply(cfg, f, val);
                return ZCS_KV_OK;
            }
        }
        return ZCS_KV_UNKNOWN_KEY;
    }
    return ZCS_KV_UNKNOWN_SECTION;
}

/* ── 公开 API ────────────────────────────────────────────────────────────── */

void zcs_config_default(zcs_config *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* [general] */
    strncpy(cfg->general.log_file, "zcs.log", sizeof(cfg->general.log_file) - 1);
    cfg->general.log_level = ZN_LOG_INFO;
    cfg->general.log_max_mb = 128; /* 默认开启轮转，防止磁盘被日志写满 */
    cfg->general.log_keep_count = 5;
    cfg->general.poller_threads = 2;
    cfg->general.stream_none_reader_delay_ms = 20000;
    strncpy(cfg->general.listen_host, "0.0.0.0", sizeof(cfg->general.listen_host) - 1);
    cfg->general.enable_vhost = 0;
    strncpy(cfg->general.extern_ip, "127.0.0.1", sizeof(cfg->general.extern_ip) - 1);
    cfg->general.buf_pool_enable = 1;
    cfg->general.buf_pool_mode = ZCS_BUF_POOL_MODE_HYBRID;
    cfg->general.buf_pool_max_per_bucket = ZCS_BUF_POOL_MAX_PER_BUCKET_DEFAULT;
    cfg->general.gop_target_gops = ZCS_GOP_QUEUE_TARGET_GOPS;
    /* 默认 5 秒时间窗；字节上限单独由 gop_max_bytes 控制（0=编译默认）。 */
    cfg->general.gop_cache_sec = 5;
    cfg->general.vod_work_threads = 2;

    /* [rtmp/rtsp/http/srt] */
    cfg->rtmp.port = 1935;
    cfg->rtsp.port = 554;
    cfg->rtsp.rtp_transport_type = -1;
    strncpy(cfg->rtsp.multicast_addr, "239.255.42.99", sizeof(cfg->rtsp.multicast_addr) - 1);
    cfg->rtsp.multicast_port_base = 0;
    cfg->rtsp.multicast_ttl = 16;
    cfg->rtsp.low_latency = 0;
    cfg->http.port = 8080;

    /* [protocol] */
    cfg->protocol.enable_rtmp = 1;
    cfg->protocol.enable_rtsp = 1;
    cfg->protocol.enable_audio = 1;
    cfg->protocol.add_mute_audio = 1;
    cfg->protocol.continue_push_ms = 15000;
    cfg->protocol.modify_stamp = 0;
    cfg->protocol.handshake_timeout_sec = 10;
    cfg->protocol.idle_timeout_sec = 60;

    /* [proxy] */
    strncpy(cfg->proxy.app, "live", sizeof(cfg->proxy.app) - 1);
    cfg->proxy.retry_count = -1;
    cfg->proxy.reconnect_delay_ms = 2000;

    /* [hls] */

    /* [auth] */
    cfg->auth.enable = 0;
    strncpy(cfg->auth.token_param, "token", sizeof(cfg->auth.token_param) - 1);

    /* [hook] */
    cfg->hook.enable = 0;
    cfg->hook.timeout_sec = 10.f;
    cfg->hook.retry_count = 1;
    cfg->hook.retry_delay_sec = 3.f;
    cfg->hook.keepalive_interval_sec = 30;
    strncpy(cfg->hook.stream_changed_schemas, "rtsp/rtmp/srt/rtp-ps/webrtc",
            sizeof(cfg->hook.stream_changed_schemas) - 1);

    /* [record] */
    strncpy(cfg->record.app, "vod", sizeof(cfg->record.app) - 1);
    strncpy(cfg->record.root, "./www/record", sizeof(cfg->record.root) - 1);
    cfg->record.file_repeat = 0;
    cfg->record.mp4_max_second = 180;
    cfg->record.max_total_mb = 0;
    cfg->record.free_space_check_mb = 0;

    /* [storage] */
    cfg->storage.enable = 0;
    strncpy(cfg->storage.dir, "./www/zstorage", sizeof(cfg->storage.dir) - 1);
    cfg->storage.capacity_gb = 2048;
    cfg->storage.retention_days = 7;
    cfg->storage.slot_size_mb = 256;
    cfg->storage.index_kb = 1024;
    cfg->storage.free_watermark_pct = 5;
    cfg->storage.flush_kb = 512;
    cfg->storage.flush_ms = 1000;
    cfg->storage.slot_max_second = 900;
    cfg->storage.seal_margin_mb = 4;
    strncpy(cfg->storage.pull_host, "127.0.0.1", sizeof(cfg->storage.pull_host) - 1);
    cfg->storage.pull_port = 554;

    /* [ssl] */
    cfg->ssl.verify_peer = 1;

    /* [onvif] */

    /* [gb28181] */

    /* [gb28181_platform] */

    /* [webrtc] */

    /* [play] */
    zcs_egress_pacing_defaults(&cfg->play.pacing);
}

zn_err_t zcs_config_load_ini(zcs_config *cfg, const char *path)
{
    if (!cfg || !path) {
        return ZN_ERR_INVALID;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        zn_warn("config: cannot open %s", path);
        return ZN_ERR_IO;
    }

    char line[1024];
    char section[64] = "general";
    unsigned unknown_keys = 0;
    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) {
                continue;
            }
            *end = '\0';
            strncpy(section, line + 1, sizeof(section) - 1);
            section[sizeof(section) - 1] = '\0';
            trim(section);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        switch (apply_kv(cfg, section, key, val)) {
        case ZCS_KV_OK:
            break;
        case ZCS_KV_UNKNOWN_SECTION:
            zn_warn("config: unknown section [%s] for key '%s' (ignored)", section, key);
            ++unknown_keys;
            break;
        case ZCS_KV_UNKNOWN_KEY:
            zn_warn("config: unknown key '%s' in [%s] (ignored; check spelling)", key, section);
            ++unknown_keys;
            break;
        }
    }
    fclose(fp);
    if (unknown_keys) {
        zn_warn("config: %u unknown entries ignored in %s", unknown_keys, path);
    }
    zn_info("config loaded: %s (rtmp=%u rtsp=%u http=%u poller=%u)", path, cfg->rtmp.port,
             cfg->rtsp.port, cfg->http.port, cfg->general.poller_threads);
    if (!cfg->http.api_secret[0]) {
        zn_warn("config: [api] secret is empty -> WebAPI only accepts local connections");
    }
    if (cfg->protocol.handshake_timeout_sec == 0 || cfg->protocol.idle_timeout_sec == 0) {
        zn_warn("config: TCP handshake/idle timeout disabled; half-open connections can "
                 "exhaust fds");
    }
    return ZN_OK;
}

int zcs_config_key_supported(const char *section, const char *key)
{
    if (!section || !key) {
        return 0;
    }
    for (const zcs_section_map *sm = k_sections; sm->section; ++sm) {
        if (strcmp(sm->section, section) != 0) {
            continue;
        }
        for (const zcs_cfg_field *f = sm->fields; f->key; ++f) {
            if (strcmp(f->key, key) == 0 || (f->alias && strcmp(f->alias, key) == 0)) {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

void zcs_config_foreach_key(void (*fn)(const char *section, const char *key, const char *alias,
                                       void *user),
                            void *user)
{
    if (!fn) {
        return;
    }
    for (const zcs_section_map *sm = k_sections; sm->section; ++sm) {
        for (const zcs_cfg_field *f = sm->fields; f->key; ++f) {
            fn(sm->section, f->key, f->alias ? f->alias : "", user);
        }
    }
}
