/**
 * @file config.c
 * @brief config.ini 默认值与加载。
 */
#include "zms/ops/config/config.h"
#include "zms/egress/pacing/egress_pacing.h"
#include "zms/core/gop/limits.h"
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
    ZMS_CFG_F_STR,   /* strncpy */
    ZMS_CFG_F_INT,   /* atoi */
    ZMS_CFG_F_UINT,  /* (unsigned)atoi */
    ZMS_CFG_F_FLOAT, /* (float)atof */
    ZMS_CFG_F_BOOL,  /* parse_bool */
    ZMS_CFG_F_CUSTOM /* 自定义解析函数 */
} zms_cfg_ftype;

typedef struct zms_cfg_field {
    const char *key;
    const char *alias; /* 旧驼峰别名，NULL 表示无别名 */
    zms_cfg_ftype type;
    size_t offset;   /* offsetof(zms_config, section.field) */
    size_t str_size; /* type==STR 时为字符串缓冲区大小 */
    void (*custom_fn)(zms_config *, const char *);
} zms_cfg_field;

/* 写入辅助 */
static void field_apply(zms_config *cfg, const zms_cfg_field *f, const char *val)
{
    void *ptr = (char *)cfg + f->offset;
    switch (f->type) {
    case ZMS_CFG_F_STR:
        strncpy((char *)ptr, val, f->str_size - 1);
        ((char *)ptr)[f->str_size - 1] = '\0';
        trim((char *)ptr);
        break;
    case ZMS_CFG_F_INT:
        *(int *)ptr = atoi(val);
        break;
    case ZMS_CFG_F_UINT:
        *(unsigned *)ptr = (unsigned)atoi(val);
        break;
    case ZMS_CFG_F_FLOAT:
        *(float *)ptr = (float)atof(val);
        break;
    case ZMS_CFG_F_BOOL:
        *(int *)ptr = parse_bool(val);
        break;
    case ZMS_CFG_F_CUSTOM:
        if (f->custom_fn) {
            f->custom_fn(cfg, val);
        }
        break;
    }
}

/* CUSTOM 解析函数 */
static void parse_log_level_fn(zms_config *cfg, const char *val)
{
    cfg->general.log_level = parse_log_level(val);
}

static void parse_buf_pool_mode_fn(zms_config *cfg, const char *val)
{
    if (strcmp(val, "global") == 0) {
        cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_GLOBAL;
    } else if (strcmp(val, "per_poller") == 0 || strcmp(val, "perPoller") == 0) {
        cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_PER_POLLER;
    } else {
        cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_HYBRID;
    }
}

/* ── section 字段表 ────────────────────────────────────────────────────── */

#define OFF(section, field) offsetof(zms_config, section.field)
#define STSZ(section, field) sizeof(((zms_config *)0)->section.field)

/* [general] */
static const zms_cfg_field k_general[] = {
    {"log_file", "logFile", ZMS_CFG_F_STR, OFF(general, log_file), STSZ(general, log_file), NULL},
    {"log_level", "logLevel", ZMS_CFG_F_CUSTOM, 0, 0, parse_log_level_fn},
    {"log_max_mb", "logMaxMB", ZMS_CFG_F_UINT, OFF(general, log_max_mb), 0, NULL},
    {"log_keep_count", "logKeepCount", ZMS_CFG_F_INT, OFF(general, log_keep_count), 0, NULL},
    {"poller_threads", "pollerThreads", ZMS_CFG_F_UINT, OFF(general, poller_threads), 0, NULL},
    {"stream_none_reader_delay_ms", "streamNoneReaderDelayMS", ZMS_CFG_F_INT,
     OFF(general, stream_none_reader_delay_ms), 0, NULL},
    {"buf_pool_enable", "bufPoolEnable", ZMS_CFG_F_BOOL, OFF(general, buf_pool_enable), 0, NULL},
    {"buf_pool_mode", "bufPoolMode", ZMS_CFG_F_CUSTOM, 0, 0, parse_buf_pool_mode_fn},
    {"buf_pool_max", "bufPoolMaxPerBucket", ZMS_CFG_F_UINT, OFF(general, buf_pool_max_per_bucket),
     0, NULL},
    {"gop_target_gops", "gopTargetGops", ZMS_CFG_F_UINT, OFF(general, gop_target_gops), 0, NULL},
    {"gop_cache_sec", "gopCacheSec", ZMS_CFG_F_UINT, OFF(general, gop_cache_sec), 0, NULL},
    {"gop_max_bytes", "gopMaxBytes", ZMS_CFG_F_UINT, OFF(general, gop_max_bytes), 0, NULL},
    {"vod_work_threads", "vodWorkThreads", ZMS_CFG_F_UINT, OFF(general, vod_work_threads), 0, NULL},
    {"extern_ip", "externIP", ZMS_CFG_F_STR, OFF(general, extern_ip), STSZ(general, extern_ip),
     NULL},
    {"listen_host", "listenHost", ZMS_CFG_F_STR, OFF(general, listen_host),
     STSZ(general, listen_host), NULL},
    {"enableVhost", "enable_vhost", ZMS_CFG_F_BOOL, OFF(general, enable_vhost), 0, NULL},
    /* media_server_id 属于 hook 但习惯放 general section */
    {"media_server_id", "mediaServerId", ZMS_CFG_F_STR, OFF(hook, media_server_id),
     STSZ(hook, media_server_id), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [rtmp] */
static const zms_cfg_field k_rtmp[] = {
    {"port", NULL, ZMS_CFG_F_UINT, OFF(rtmp, port), 0, NULL},
    {"sslport", NULL, ZMS_CFG_F_UINT, OFF(rtmp, ssl_port), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [rtsp] */
static const zms_cfg_field k_rtsp[] = {
    {"port", NULL, ZMS_CFG_F_UINT, OFF(rtsp, port), 0, NULL},
    {"sslport", NULL, ZMS_CFG_F_UINT, OFF(rtsp, ssl_port), 0, NULL},
    {"auth_user", NULL, ZMS_CFG_F_STR, OFF(rtsp, auth_user), STSZ(rtsp, auth_user), NULL},
    {"auth_pass", NULL, ZMS_CFG_F_STR, OFF(rtsp, auth_pass), STSZ(rtsp, auth_pass), NULL},
    {"test_reject_tcp_setup", NULL, ZMS_CFG_F_BOOL, OFF(rtsp, test_reject_tcp_setup), 0, NULL},
    {"rtpTransportType", "rtp_transport_type", ZMS_CFG_F_INT, OFF(rtsp, rtp_transport_type), 0, NULL},
    {"multicast_addr", "multicastAddr", ZMS_CFG_F_STR, OFF(rtsp, multicast_addr),
     STSZ(rtsp, multicast_addr), NULL},
    {"multicast_port_base", "multicastPortBase", ZMS_CFG_F_UINT, OFF(rtsp, multicast_port_base), 0,
     NULL},
    {"multicast_ttl", "multicastTtl", ZMS_CFG_F_INT, OFF(rtsp, multicast_ttl), 0, NULL},
    {"low_latency", "lowLatency", ZMS_CFG_F_BOOL, OFF(rtsp, low_latency), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [http] */
static const zms_cfg_field k_http[] = {
    {"port", NULL, ZMS_CFG_F_UINT, OFF(http, port), 0, NULL},
    {"sslport", NULL, ZMS_CFG_F_UINT, OFF(http, ssl_port), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [api] api.secret 逻辑上属于 http */
static const zms_cfg_field k_api[] = {
    {"secret", NULL, ZMS_CFG_F_STR, OFF(http, api_secret), STSZ(http, api_secret), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [srt] */
static const zms_cfg_field k_srt[] = {
    {"port", NULL, ZMS_CFG_F_UINT, OFF(srt, port), 0, NULL},
    {"timeoutSec", "timeout_sec", ZMS_CFG_F_INT, OFF(srt, timeout_sec), 0, NULL},
    {"latencyMul", "latency_mul", ZMS_CFG_F_INT, OFF(srt, latency_mul), 0, NULL},
    {"pktBufSize", "pkt_buf_size", ZMS_CFG_F_INT, OFF(srt, pkt_buf_size), 0, NULL},
    {"passPhrase", "passphrase", ZMS_CFG_F_STR, OFF(srt, passphrase), STSZ(srt, passphrase), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [webrtc] */
static const zms_cfg_field k_webrtc[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, OFF(webrtc, enable), 0, NULL},
    {"port_min", NULL, ZMS_CFG_F_UINT, OFF(webrtc, port_min), 0, NULL},
    {"port_max", NULL, ZMS_CFG_F_UINT, OFF(webrtc, port_max), 0, NULL},
    {"max_sessions", "maxSessions", ZMS_CFG_F_UINT, OFF(webrtc, max_sessions), 0, NULL},
    {"stun_server", "stunServer", ZMS_CFG_F_STR, OFF(webrtc, stun_server),
     STSZ(webrtc, stun_server), NULL},
    {"turn_server", "turnServer", ZMS_CFG_F_STR, OFF(webrtc, turn_server),
     STSZ(webrtc, turn_server), NULL},
    {"turn_user", "turnUser", ZMS_CFG_F_STR, OFF(webrtc, turn_user), STSZ(webrtc, turn_user), NULL},
    {"turn_pass", "turnPass", ZMS_CFG_F_STR, OFF(webrtc, turn_pass), STSZ(webrtc, turn_pass), NULL},
    {"signaling_enable", "signalingEnable", ZMS_CFG_F_BOOL, OFF(webrtc, signaling_enable), 0, NULL},
    {"signaling_url", "signalingUrl", ZMS_CFG_F_STR, OFF(webrtc, signaling_url),
     STSZ(webrtc, signaling_url), NULL},
    {"signaling_room", "signalingRoom", ZMS_CFG_F_STR, OFF(webrtc, signaling_room),
     STSZ(webrtc, signaling_room), NULL},
    {"signaling_token", "signalingToken", ZMS_CFG_F_STR, OFF(webrtc, signaling_token),
     STSZ(webrtc, signaling_token), NULL},
    {"signaling_app", "signalingApp", ZMS_CFG_F_STR, OFF(webrtc, signaling_app),
     STSZ(webrtc, signaling_app), NULL},
    {"signaling_stream", "signalingStream", ZMS_CFG_F_STR, OFF(webrtc, signaling_stream),
     STSZ(webrtc, signaling_stream), NULL},
    {"whep_client_url", "whepClientUrl", ZMS_CFG_F_STR, OFF(webrtc, whep_client_url),
     STSZ(webrtc, whep_client_url), NULL},
    {"whep_client_app", "whepClientApp", ZMS_CFG_F_STR, OFF(webrtc, whep_client_app),
     STSZ(webrtc, whep_client_app), NULL},
    {"whep_client_stream", "whepClientStream", ZMS_CFG_F_STR, OFF(webrtc, whep_client_stream),
     STSZ(webrtc, whep_client_stream), NULL},
    {"whep_client_token", "whepClientToken", ZMS_CFG_F_STR, OFF(webrtc, whep_client_token),
     STSZ(webrtc, whep_client_token), NULL},
    {"whip_client_url", "whipClientUrl", ZMS_CFG_F_STR, OFF(webrtc, whip_client_url),
     STSZ(webrtc, whip_client_url), NULL},
    {"whip_client_app", "whipClientApp", ZMS_CFG_F_STR, OFF(webrtc, whip_client_app),
     STSZ(webrtc, whip_client_app), NULL},
    {"whip_client_stream", "whipClientStream", ZMS_CFG_F_STR, OFF(webrtc, whip_client_stream),
     STSZ(webrtc, whip_client_stream), NULL},
    {"whip_client_token", "whipClientToken", ZMS_CFG_F_STR, OFF(webrtc, whip_client_token),
     STSZ(webrtc, whip_client_token), NULL},
    /* advertise_host / externIP 覆盖 general.extern_ip（保持旧行为） */
    {"advertise_host", "externIP", ZMS_CFG_F_STR, OFF(general, extern_ip), STSZ(general, extern_ip),
     NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [protocol] */
static const zms_cfg_field k_protocol[] = {
    {"enable_rtmp", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_rtmp), 0, NULL},
    {"enable_rtsp", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_rtsp), 0, NULL},
    {"enable_srt", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_srt), 0, NULL},
    {"enable_hls", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_hls), 0, NULL},
    {"enable_hls_fmp4", "enableHlsFmp4", ZMS_CFG_F_BOOL, OFF(protocol, enable_hls_fmp4), 0, NULL},
    {"hls_demand", "hlsDemand", ZMS_CFG_F_BOOL, OFF(protocol, hls_demand), 0, NULL},
    {"dash_demand", "dashDemand", ZMS_CFG_F_BOOL, OFF(protocol, dash_demand), 0, NULL},
    {"hls_save_path", "hlsSavePath", ZMS_CFG_F_STR, OFF(protocol, hls_save_path),
     STSZ(protocol, hls_save_path), NULL},
    {"enable_audio", NULL, ZMS_CFG_F_BOOL, OFF(protocol, enable_audio), 0, NULL},
    {"add_mute_audio", "addMuteAudio", ZMS_CFG_F_BOOL, OFF(protocol, add_mute_audio), 0, NULL},
    {"continue_push_ms", "continuePushMs", ZMS_CFG_F_INT, OFF(protocol, continue_push_ms), 0, NULL},
    {"modify_stamp", "modifyStamp", ZMS_CFG_F_INT, OFF(protocol, modify_stamp), 0, NULL},
    {"handshake_timeout_sec", "handshakeTimeoutSec", ZMS_CFG_F_INT,
     OFF(protocol, handshake_timeout_sec), 0, NULL},
    {"idle_timeout_sec", "idleTimeoutSec", ZMS_CFG_F_INT, OFF(protocol, idle_timeout_sec), 0, NULL},
    {"max_sessions", "maxSessions", ZMS_CFG_F_INT, OFF(protocol, max_sessions), 0, NULL},
    {"max_sessions_per_ip", "maxSessionsPerIp", ZMS_CFG_F_INT,
     OFF(protocol, max_sessions_per_ip), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [hls] */
static const zms_cfg_field k_hls[] = {
    {"seg_dur", "segDur", ZMS_CFG_F_FLOAT, OFF(hls, segment_duration_sec), 0, NULL},
    {"seg_num", "segNum", ZMS_CFG_F_UINT, OFF(hls, segment_count), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [proxy] */
static const zms_cfg_field k_proxy[] = {
    {"proxy_pull", "pull_url", ZMS_CFG_F_STR, OFF(proxy, pull), STSZ(proxy, pull), NULL},
    {"proxy_app", "app", ZMS_CFG_F_STR, OFF(proxy, app), STSZ(proxy, app), NULL},
    {"proxy_stream", "stream", ZMS_CFG_F_STR, OFF(proxy, stream), STSZ(proxy, stream), NULL},
    {"proxy_prefix", "prefix", ZMS_CFG_F_STR, OFF(proxy, prefix), STSZ(proxy, prefix), NULL},
    {"retry_count", NULL, ZMS_CFG_F_INT, OFF(proxy, retry_count), 0, NULL},
    {"reconnect_delay_ms", NULL, ZMS_CFG_F_INT, OFF(proxy, reconnect_delay_ms), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [record] */
static const zms_cfg_field k_record[] = {
    {"app", "appName", ZMS_CFG_F_STR, OFF(record, app), STSZ(record, app), NULL},
    {"root", "mp4_save_path", ZMS_CFG_F_STR, OFF(record, root), STSZ(record, root), NULL},
    {"file_repeat", "fileRepeat", ZMS_CFG_F_BOOL, OFF(record, file_repeat), 0, NULL},
    {"mp4_max_second", "mp4MaxSecond", ZMS_CFG_F_INT, OFF(record, mp4_max_second), 0, NULL},
    {"max_total_mb", "maxTotalMb", ZMS_CFG_F_INT, OFF(record, max_total_mb), 0, NULL},
    {"free_space_check_mb", "freeSpaceCheckMb", ZMS_CFG_F_INT, OFF(record, free_space_check_mb), 0,
     NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [storage] 中心云存储 */
static const zms_cfg_field k_storage[] = {
    {"enable", "storageEnable", ZMS_CFG_F_BOOL, OFF(storage, enable), 0, NULL},
    {"dir", "storageDir", ZMS_CFG_F_STR, OFF(storage, dir), STSZ(storage, dir), NULL},
    {"capacity_gb", "capacityGb", ZMS_CFG_F_INT, OFF(storage, capacity_gb), 0, NULL},
    {"retention_days", "retentionDays", ZMS_CFG_F_INT, OFF(storage, retention_days), 0, NULL},
    {"slot_size_mb", "slotSizeMb", ZMS_CFG_F_INT, OFF(storage, slot_size_mb), 0, NULL},
    {"index_kb", "indexKb", ZMS_CFG_F_INT, OFF(storage, index_kb), 0, NULL},
    {"free_watermark_pct", "freeWatermarkPct", ZMS_CFG_F_INT, OFF(storage, free_watermark_pct), 0,
     NULL},
    {"flush_kb", "flushKb", ZMS_CFG_F_INT, OFF(storage, flush_kb), 0, NULL},
    {"flush_ms", "flushMs", ZMS_CFG_F_INT, OFF(storage, flush_ms), 0, NULL},
    {"slot_max_second", "slotMaxSecond", ZMS_CFG_F_INT, OFF(storage, slot_max_second), 0, NULL},
    {"seal_margin_mb", "sealMarginMb", ZMS_CFG_F_INT, OFF(storage, seal_margin_mb), 0, NULL},
    {"pull_host", "pullHost", ZMS_CFG_F_STR, OFF(storage, pull_host), STSZ(storage, pull_host), NULL},
    {"pull_port", "pullPort", ZMS_CFG_F_INT, OFF(storage, pull_port), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [play] 所有字段属于 play.pacing */
#define POFF(field) offsetof(zms_config, play.pacing.field)
static const zms_cfg_field k_play[] = {
    {"ring_max_lag", "ringMaxLag", ZMS_CFG_F_UINT, POFF(ring_max_lag), 0, NULL},
    {"catchup_lag", "catchupLag", ZMS_CFG_F_UINT, POFF(catchup_lag), 0, NULL},
    {"frame_budget_live", "frameBudgetLive", ZMS_CFG_F_UINT, POFF(frame_budget_live), 0, NULL},
    {"frame_budget_catchup", "frameBudgetCatchup", ZMS_CFG_F_UINT, POFF(frame_budget_catchup), 0,
     NULL},
    {"burst_budget", "burstBudget", ZMS_CFG_F_UINT, POFF(burst_budget), 0, NULL},
    {"flv_budget_live", "flvBudgetLive", ZMS_CFG_F_UINT, POFF(flv_budget_live), 0, NULL},
    {"flv_budget_vod", "flvBudgetVod", ZMS_CFG_F_UINT, POFF(flv_budget_vod), 0, NULL},
    {"pace_lead_ms", "paceLeadMs", ZMS_CFG_F_UINT, POFF(pace_lead_ms), 0, NULL},
    {"resync_lag", "resyncLag", ZMS_CFG_F_UINT, POFF(resync_lag), 0, NULL},
    {"resync_lag_max", "resyncLagMax", ZMS_CFG_F_UINT, POFF(resync_lag_max), 0, NULL},
    {"resync_cooldown_ms", "resyncCooldownMs", ZMS_CFG_F_UINT, POFF(resync_cooldown_ms), 0, NULL},
    {"slow_consumer_kick_lag", "slowConsumerKickLag", ZMS_CFG_F_UINT, POFF(slow_consumer_kick_lag),
     0, NULL},
    {"vod_catchup_start", "vodCatchupStart", ZMS_CFG_F_UINT, POFF(vod_catchup_start), 0, NULL},
    {"vod_catchup_seek", "vodCatchupSeek", ZMS_CFG_F_UINT, POFF(vod_catchup_seek), 0, NULL},
    {"vod_prefill_lag", "vodPrefillLag", ZMS_CFG_F_UINT, POFF(vod_prefill_lag), 0, NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};
#undef POFF

/* [hook] */
#define HOFF(field) offsetof(zms_config, hook.field)
#define HSSZ(field) sizeof(((zms_config *)0)->hook.field)
static const zms_cfg_field k_hook[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, HOFF(enable), 0, NULL},
    {"timeout_sec", "timeoutSec", ZMS_CFG_F_FLOAT, HOFF(timeout_sec), 0, NULL},
    {"retry", NULL, ZMS_CFG_F_INT, HOFF(retry_count), 0, NULL},
    {"retry_delay", NULL, ZMS_CFG_F_FLOAT, HOFF(retry_delay_sec), 0, NULL},
    {"on_stream_changed", NULL, ZMS_CFG_F_STR, HOFF(on_stream_changed), HSSZ(on_stream_changed),
     NULL},
    {"on_server_started", NULL, ZMS_CFG_F_STR, HOFF(on_server_started), HSSZ(on_server_started),
     NULL},
    {"on_server_keepalive", NULL, ZMS_CFG_F_STR, HOFF(on_server_keepalive),
     HSSZ(on_server_keepalive), NULL},
    {"keepalive_interval_sec", "keepaliveIntervalSec", ZMS_CFG_F_INT,
     HOFF(keepalive_interval_sec), 0, NULL},
    {"on_stream_none_reader", NULL, ZMS_CFG_F_STR, HOFF(on_stream_none_reader),
     HSSZ(on_stream_none_reader), NULL},
    {"on_play_stop", NULL, ZMS_CFG_F_STR, HOFF(on_play_stop), HSSZ(on_play_stop), NULL},
    {"on_record_mp4", NULL, ZMS_CFG_F_STR, HOFF(on_record_mp4), HSSZ(on_record_mp4), NULL},
    {"stream_changed_schemas", NULL, ZMS_CFG_F_STR, HOFF(stream_changed_schemas),
     HSSZ(stream_changed_schemas), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};
#undef HOFF
#undef HSSZ

/* [auth] */
#define AOFF(field) offsetof(zms_config, auth.field)
#define ASSZ(field) sizeof(((zms_config *)0)->auth.field)
static const zms_cfg_field k_auth[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, AOFF(enable), 0, NULL},
    {"token_param", "tokenParam", ZMS_CFG_F_STR, AOFF(token_param), ASSZ(token_param), NULL},
    {"secret", NULL, ZMS_CFG_F_STR, AOFF(secret), ASSZ(secret), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};
#undef AOFF
#undef ASSZ

/* [ssl] */
static const zms_cfg_field k_ssl[] = {
    {"cert_file", "certFile", ZMS_CFG_F_STR, OFF(ssl, cert_file), STSZ(ssl, cert_file), NULL},
    {"key_file", "keyFile", ZMS_CFG_F_STR, OFF(ssl, key_file), STSZ(ssl, key_file), NULL},
    {"ca_file", "caFile", ZMS_CFG_F_STR, OFF(ssl, ca_file), STSZ(ssl, ca_file), NULL},
    {"verify_peer", "verifyPeer", ZMS_CFG_F_BOOL, OFF(ssl, verify_peer), 0, NULL},
    {"client_cert", NULL, ZMS_CFG_F_STR, OFF(ssl, client_cert), STSZ(ssl, client_cert), NULL},
    {"client_key", NULL, ZMS_CFG_F_STR, OFF(ssl, client_key), STSZ(ssl, client_key), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [onvif] */
static const zms_cfg_field k_onvif[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, OFF(onvif, enable), 0, NULL},
    {"server_enable", "serverEnable", ZMS_CFG_F_BOOL, OFF(onvif, server_enable), 0, NULL},
    {"server_name", "serverName", ZMS_CFG_F_STR, OFF(onvif, server_name),
     STSZ(onvif, server_name), NULL},
    {"server_uuid", "serverUuid", ZMS_CFG_F_STR, OFF(onvif, server_uuid),
     STSZ(onvif, server_uuid), NULL},
    {"default_vhost", "defaultVhost", ZMS_CFG_F_STR, OFF(onvif, default_vhost),
     STSZ(onvif, default_vhost), NULL},
    {"default_app", "defaultApp", ZMS_CFG_F_STR, OFF(onvif, default_app),
     STSZ(onvif, default_app), NULL},
    {"auth_user", "authUser", ZMS_CFG_F_STR, OFF(onvif, auth_user), STSZ(onvif, auth_user), NULL},
    {"auth_pass", "authPass", ZMS_CFG_F_STR, OFF(onvif, auth_pass), STSZ(onvif, auth_pass), NULL},
    {"timeout_ms", "timeoutMs", ZMS_CFG_F_INT, OFF(onvif, timeout_ms), 0, NULL},
    {"retry_count", "retryCount", ZMS_CFG_F_INT, OFF(onvif, retry_count), 0, NULL},
    {"reconnect_delay_ms", "reconnectDelayMs", ZMS_CFG_F_INT, OFF(onvif, reconnect_delay_ms), 0,
     NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [gb28181] */
static const zms_cfg_field k_gb28181[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, OFF(gb28181, enable), 0, NULL},
    {"platform_id", "platformId", ZMS_CFG_F_STR, OFF(gb28181, platform_id),
     STSZ(gb28181, platform_id), NULL},
    {"platform_domain", "platformDomain", ZMS_CFG_F_STR, OFF(gb28181, platform_domain),
     STSZ(gb28181, platform_domain), NULL},
    {"platform_ip", "platformIp", ZMS_CFG_F_STR, OFF(gb28181, platform_ip),
     STSZ(gb28181, platform_ip), NULL},
    {"platform_port", "platformPort", ZMS_CFG_F_UINT, OFF(gb28181, platform_port), 0, NULL},
    {"device_id", "deviceId", ZMS_CFG_F_STR, OFF(gb28181, device_id), STSZ(gb28181, device_id),
     NULL},
    {"device_name", "deviceName", ZMS_CFG_F_STR, OFF(gb28181, device_name),
     STSZ(gb28181, device_name), NULL},
    {"manufacturer", NULL, ZMS_CFG_F_STR, OFF(gb28181, manufacturer),
     STSZ(gb28181, manufacturer), NULL},
    {"model", NULL, ZMS_CFG_F_STR, OFF(gb28181, model), STSZ(gb28181, model), NULL},
    {"firmware", NULL, ZMS_CFG_F_STR, OFF(gb28181, firmware), STSZ(gb28181, firmware), NULL},
    {"username", NULL, ZMS_CFG_F_STR, OFF(gb28181, username), STSZ(gb28181, username), NULL},
    {"password", NULL, ZMS_CFG_F_STR, OFF(gb28181, password), STSZ(gb28181, password), NULL},
    {"local_ip", "localIp", ZMS_CFG_F_STR, OFF(gb28181, local_ip), STSZ(gb28181, local_ip), NULL},
    {"local_port", "localPort", ZMS_CFG_F_UINT, OFF(gb28181, local_port), 0, NULL},
    {"sip_transport", "sipTransport", ZMS_CFG_F_STR, OFF(gb28181, sip_transport),
     STSZ(gb28181, sip_transport), NULL},
    {"sip_mode", "sipMode", ZMS_CFG_F_STR, OFF(gb28181, sip_mode), STSZ(gb28181, sip_mode), NULL},
    {"tls_ca", "tlsCa", ZMS_CFG_F_STR, OFF(gb28181, tls_ca), STSZ(gb28181, tls_ca), NULL},
    {"tls_cert", "tlsCert", ZMS_CFG_F_STR, OFF(gb28181, tls_cert), STSZ(gb28181, tls_cert), NULL},
    {"tls_key", "tlsKey", ZMS_CFG_F_STR, OFF(gb28181, tls_key), STSZ(gb28181, tls_key), NULL},
    {"tls_verify_peer", "tlsVerifyPeer", ZMS_CFG_F_BOOL, OFF(gb28181, tls_verify_peer), 0, NULL},
    {"register_expires", "registerExpires", ZMS_CFG_F_INT, OFF(gb28181, register_expires), 0, NULL},
    {"keepalive_interval", "keepaliveInterval", ZMS_CFG_F_INT,
     OFF(gb28181, keepalive_interval), 0, NULL},
    {"channels", NULL, ZMS_CFG_F_STR, OFF(gb28181, channels), STSZ(gb28181, channels), NULL},
    {"record_root", "recordRoot", ZMS_CFG_F_STR, OFF(gb28181, record_root),
     STSZ(gb28181, record_root), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

/* [gb28181_platform] */
static const zms_cfg_field k_gb28181_platform[] = {
    {"enable", NULL, ZMS_CFG_F_BOOL, OFF(gb28181_platform, enable), 0, NULL},
    {"id", NULL, ZMS_CFG_F_STR, OFF(gb28181_platform, id), STSZ(gb28181_platform, id), NULL},
    {"domain", NULL, ZMS_CFG_F_STR, OFF(gb28181_platform, domain),
     STSZ(gb28181_platform, domain), NULL},
    {"password", NULL, ZMS_CFG_F_STR, OFF(gb28181_platform, password),
     STSZ(gb28181_platform, password), NULL},
    {"advertise_ip", "advertiseIp", ZMS_CFG_F_STR, OFF(gb28181_platform, advertise_ip),
     STSZ(gb28181_platform, advertise_ip), NULL},
    {"listen_port", "listenPort", ZMS_CFG_F_UINT, OFF(gb28181_platform, listen_port), 0, NULL},
    {"transport", NULL, ZMS_CFG_F_STR, OFF(gb28181_platform, transport),
     STSZ(gb28181_platform, transport), NULL},
    {"tls_cert", "tlsCert", ZMS_CFG_F_STR, OFF(gb28181_platform, tls_cert),
     STSZ(gb28181_platform, tls_cert), NULL},
    {"tls_key", "tlsKey", ZMS_CFG_F_STR, OFF(gb28181_platform, tls_key),
     STSZ(gb28181_platform, tls_key), NULL},
    {"tls_ca", "tlsCa", ZMS_CFG_F_STR, OFF(gb28181_platform, tls_ca),
     STSZ(gb28181_platform, tls_ca), NULL},
    {"tls_verify_peer", "tlsVerifyPeer", ZMS_CFG_F_BOOL, OFF(gb28181_platform, tls_verify_peer),
     0, NULL},
    {"keepalive_timeout_sec", "keepaliveTimeoutSec", ZMS_CFG_F_INT,
     OFF(gb28181_platform, keepalive_timeout_sec), 0, NULL},
    {"media_vhost", "mediaVhost", ZMS_CFG_F_STR, OFF(gb28181_platform, media_vhost),
     STSZ(gb28181_platform, media_vhost), NULL},
    {"media_app", "mediaApp", ZMS_CFG_F_STR, OFF(gb28181_platform, media_app),
     STSZ(gb28181_platform, media_app), NULL},
    {NULL, NULL, ZMS_CFG_F_STR, 0, 0, NULL}};

#undef OFF
#undef STSZ

/* ── section 字段表映射 ─────────────────────────────────────────────────── */

typedef struct {
    const char *section;
    const zms_cfg_field *fields;
} zms_section_map;

static const zms_section_map k_sections[] = {
    {"general", k_general}, {"rtmp", k_rtmp},   {"rtsp", k_rtsp},     {"http", k_http},
    {"api", k_api},         {"srt", k_srt},     {"webrtc", k_webrtc}, {"protocol", k_protocol},
    {"hls", k_hls},         {"proxy", k_proxy}, {"record", k_record}, {"play", k_play},
    {"hook", k_hook},       {"auth", k_auth},   {"ssl", k_ssl},       {"onvif", k_onvif},
    {"storage", k_storage},
    {"gb28181", k_gb28181},
    {"gb28181_platform", k_gb28181_platform},   {NULL, NULL}};

/* ── apply_kv：查表写入 ───────────────────────────────────────────────────── */

/** 键值写入结果，用于在加载时提示拼写错误（否则配置静默失效，最难排查）。 */
typedef enum {
    ZMS_KV_OK = 0,
    ZMS_KV_UNKNOWN_SECTION,
    ZMS_KV_UNKNOWN_KEY,
} zms_kv_result;

static zms_kv_result apply_kv(zms_config *cfg, const char *section, const char *key,
                              const char *val)
{
    if (!cfg || !section || !key || !val) {
        return ZMS_KV_UNKNOWN_SECTION;
    }
    for (const zms_section_map *sm = k_sections; sm->section; ++sm) {
        if (strcmp(sm->section, section) != 0) {
            continue;
        }
        for (const zms_cfg_field *f = sm->fields; f->key; ++f) {
            if (strcmp(f->key, key) == 0 || (f->alias && strcmp(f->alias, key) == 0)) {
                field_apply(cfg, f, val);
                return ZMS_KV_OK;
            }
        }
        return ZMS_KV_UNKNOWN_KEY;
    }
    return ZMS_KV_UNKNOWN_SECTION;
}

/* ── 公开 API ────────────────────────────────────────────────────────────── */

void zms_config_default(zms_config *cfg)
{
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* [general] */
    strncpy(cfg->general.log_file, "zms_media_server.log", sizeof(cfg->general.log_file) - 1);
    cfg->general.log_level = ZN_LOG_INFO;
    cfg->general.log_max_mb = 128; /* 默认开启轮转，防止磁盘被日志写满 */
    cfg->general.log_keep_count = 5;
    cfg->general.poller_threads = 2;
    cfg->general.stream_none_reader_delay_ms = 20000;
    strncpy(cfg->general.listen_host, "0.0.0.0", sizeof(cfg->general.listen_host) - 1);
    cfg->general.enable_vhost = 0;
    strncpy(cfg->general.extern_ip, "127.0.0.1", sizeof(cfg->general.extern_ip) - 1);
    cfg->general.buf_pool_enable = 1;
    cfg->general.buf_pool_mode = ZMS_BUF_POOL_MODE_HYBRID;
    cfg->general.buf_pool_max_per_bucket = ZMS_BUF_POOL_MAX_PER_BUCKET_DEFAULT;
    cfg->general.gop_target_gops = ZMS_GOP_QUEUE_TARGET_GOPS;
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
    cfg->srt.port = 9000;
    cfg->srt.timeout_sec = 5;
    cfg->srt.latency_mul = 4;
    cfg->srt.pkt_buf_size = 8192;
    cfg->http.port = 8080;

    /* [protocol] */
    cfg->protocol.enable_rtmp = 1;
    cfg->protocol.enable_rtsp = 1;
    cfg->protocol.enable_srt = 1;
    cfg->protocol.enable_hls = 1;
    cfg->protocol.enable_hls_fmp4 = 0;
    cfg->protocol.hls_demand = 0;
    cfg->protocol.dash_demand = 0;
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
    cfg->hls.segment_duration_sec = 2.f;
    cfg->hls.segment_count = 3;

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
    cfg->onvif.enable = 0;
    cfg->onvif.server_enable = 0;
    strncpy(cfg->onvif.server_name, "ZMS", sizeof(cfg->onvif.server_name) - 1);
    strncpy(cfg->onvif.default_app, "onvif", sizeof(cfg->onvif.default_app) - 1);
    cfg->onvif.timeout_ms = 5000;
    cfg->onvif.retry_count = -1;
    cfg->onvif.reconnect_delay_ms = 3000;

    /* [gb28181] */
    cfg->gb28181.enable = 0;
    cfg->gb28181.platform_port = 5060;
    cfg->gb28181.local_port = 5060;
    strncpy(cfg->gb28181.sip_transport, "udp", sizeof(cfg->gb28181.sip_transport) - 1);
    strncpy(cfg->gb28181.sip_mode, "client", sizeof(cfg->gb28181.sip_mode) - 1);
    cfg->gb28181.register_expires = 3600;
    cfg->gb28181.keepalive_interval = 60;
    strncpy(cfg->gb28181.device_name, "ZMS-IPC", sizeof(cfg->gb28181.device_name) - 1);
    strncpy(cfg->gb28181.manufacturer, "Zero", sizeof(cfg->gb28181.manufacturer) - 1);
    strncpy(cfg->gb28181.model, "ZMS", sizeof(cfg->gb28181.model) - 1);
    strncpy(cfg->gb28181.firmware, "0.5.0", sizeof(cfg->gb28181.firmware) - 1);

    /* [gb28181_platform] */
    cfg->gb28181_platform.enable = 0;
    cfg->gb28181_platform.listen_port = 5060;
    strncpy(cfg->gb28181_platform.transport, "udp",
            sizeof(cfg->gb28181_platform.transport) - 1);
    cfg->gb28181_platform.keepalive_timeout_sec = 180;
    strncpy(cfg->gb28181_platform.media_app, "gb28181",
            sizeof(cfg->gb28181_platform.media_app) - 1);

    /* [webrtc] */
    cfg->webrtc.enable = 1;
    cfg->webrtc.port_min = 50000;
    cfg->webrtc.port_max = 60000;
    cfg->webrtc.max_sessions = 64;
    cfg->webrtc.signaling_enable = 0;
    strncpy(cfg->webrtc.signaling_room, "door-1", sizeof(cfg->webrtc.signaling_room) - 1);
    strncpy(cfg->webrtc.signaling_app, "live", sizeof(cfg->webrtc.signaling_app) - 1);
    strncpy(cfg->webrtc.signaling_stream, "cam1", sizeof(cfg->webrtc.signaling_stream) - 1);

    /* [play] */
    zms_egress_pacing_defaults(&cfg->play.pacing);
}

zn_err_t zms_config_load_ini(zms_config *cfg, const char *path)
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
        case ZMS_KV_OK:
            break;
        case ZMS_KV_UNKNOWN_SECTION:
            zn_warn("config: unknown section [%s] for key '%s' (ignored)", section, key);
            ++unknown_keys;
            break;
        case ZMS_KV_UNKNOWN_KEY:
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

int zms_config_key_supported(const char *section, const char *key)
{
    if (!section || !key) {
        return 0;
    }
    for (const zms_section_map *sm = k_sections; sm->section; ++sm) {
        if (strcmp(sm->section, section) != 0) {
            continue;
        }
        for (const zms_cfg_field *f = sm->fields; f->key; ++f) {
            if (strcmp(f->key, key) == 0 || (f->alias && strcmp(f->alias, key) == 0)) {
                return 1;
            }
        }
        return 0;
    }
    return 0;
}

void zms_config_foreach_key(void (*fn)(const char *section, const char *key, const char *alias,
                                       void *user),
                            void *user)
{
    if (!fn) {
        return;
    }
    for (const zms_section_map *sm = k_sections; sm->section; ++sm) {
        for (const zms_cfg_field *f = sm->fields; f->key; ++f) {
            fn(sm->section, f->key, f->alias ? f->alias : "", user);
        }
    }
}
