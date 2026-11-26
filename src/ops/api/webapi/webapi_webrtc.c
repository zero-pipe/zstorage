/**
 * @file webapi_webrtc.c
 * @brief Web API WebRTC：统计、能力、主动客户端管理。
 */
#include "ops/api/webapi/webapi_internal.h"
#include "zms/media/media_limits.h"
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
#include "zms/session/webrtc/webrtc_server.h"
#include "zms/ops/config/pull_ssl.h"
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC

size_t webapi_handle_webrtc_stats(char *body, size_t body_cap, int *http_status)
{
    /* 每会话统计 ~520B：按当前生效上限分配，堆上取，别放 poller 栈。 */
    size_t cap = zms_webrtc_session_limit();
    zms_webrtc_session_stats *st = (zms_webrtc_session_stats *)calloc(cap, sizeof(*st));
    zms_webapi_json_buf jb = {body, body_cap, 0};
    size_t n;
    size_t i;
    uint64_t total_rtpq_bytes = 0;
    uint64_t total_nack_bytes = 0;
    uint64_t total_whip_rx_bytes = 0;

    if (!st) {
        *http_status = 500;
        return webapi_respond_json(ZMS_API_OTHER_FAILED, "out of memory", NULL, body, body_cap);
    }
    n = zms_webrtc_session_stats_all(st, cap);
    if (webapi_jb_append(&jb, "{\"code\":0,\"data\":[") != 0) {
        goto fail;
    }
    for (i = 0; i < n; ++i) {
        total_rtpq_bytes += st[i].rtpq_bytes;
        total_nack_bytes += st[i].nack_cache_bytes;
        total_whip_rx_bytes += st[i].whip_rx_bytes;
        if (webapi_jb_append(&jb,
                      "%s{\"id\":\"%s\",\"app\":\"%s\",\"stream\":\"%s\",\"session\":%u,"
                      "\"mode\":%d,\"rtpqPkts\":%u,\"rtpqBytes\":%llu,\"rtpqBytesMax\":%llu,"
                      "\"rtpqDroppedBytes\":%llu,\"rtpqDroppedPkts\":%llu,"
                      "\"rtpqAllocFail\":%llu,\"nackCachePkts\":%u,\"nackCacheBytes\":%llu,"
                      "\"nackHits\":%llu,\"nackMisses\":%llu,\"nackRetransmits\":%llu,"
                      "\"rtcpRrPkts\":%llu,\"rtcpFractionLost\":%u,\"rtcpJitter\":%u,"
                      "\"rtcpRttMs\":%lld,\"twccExtId\":%d,\"twccFbPkts\":%llu,"
                      "\"twccReceived\":%llu,\"twccLost\":%llu,\"twccLossPermille\":%u,"
                      "\"twccLossEwma\":%u,\"sendBps\":%u,\"paceWaits\":%llu,"
                      "\"adaptBps\":%u,\"adaptDropping\":%d,\"adaptWaitMs\":%llu,"
                      "\"adaptResets\":%llu,\"adaptDroppedFrames\":%llu,\"adaptDroppedBytes\":%llu,"
                      "\"pliCount\":%llu,\"pliSuppressed\":%llu,"
                      "\"talkEnabled\":%d,\"talkRxPkts\":%llu,\"talkRxBytes\":%llu,"
                      "\"whipRxPkts\":%llu,\"whipRxBytes\":%llu,"
                      "\"whipRxDropped\":%llu,\"whipSrtpFail\":%llu,"
                      "\"createdMs\":%llu,\"lastRxMs\":%llu}",
                      i == 0 ? "" : ",", st[i].id, st[i].app, st[i].stream, st[i].session_no,
                      st[i].mode, st[i].rtpq_pkts, (unsigned long long)st[i].rtpq_bytes,
                      (unsigned long long)st[i].rtpq_bytes_max,
                      (unsigned long long)st[i].rtpq_dropped_bytes,
                      (unsigned long long)st[i].rtpq_dropped_pkts,
                      (unsigned long long)st[i].rtpq_alloc_fail, st[i].nack_cache_pkts,
                      (unsigned long long)st[i].nack_cache_bytes,
                      (unsigned long long)st[i].nack_hits,
                      (unsigned long long)st[i].nack_misses,
                      (unsigned long long)st[i].nack_retransmits,
                      (unsigned long long)st[i].rtcp_rr_pkts, st[i].rtcp_fraction_lost,
                      st[i].rtcp_jitter, (long long)st[i].rtcp_rtt_ms,
                      st[i].twcc_ext_id, (unsigned long long)st[i].twcc_fb_pkts,
                      (unsigned long long)st[i].twcc_received,
                      (unsigned long long)st[i].twcc_lost, (unsigned)st[i].twcc_loss_permille,
                      (unsigned)st[i].twcc_loss_ewma, st[i].send_bps,
                      (unsigned long long)st[i].pace_waits, st[i].adapt_bps,
                      st[i].adapt_dropping, (unsigned long long)st[i].adapt_wait_ms,
                      (unsigned long long)st[i].adapt_resets,
                      (unsigned long long)st[i].adapt_dropped_frames,
                      (unsigned long long)st[i].adapt_dropped_bytes,
                      (unsigned long long)st[i].pli_count,
                      (unsigned long long)st[i].pli_suppressed, st[i].talk_enabled,
                      (unsigned long long)st[i].talk_rx_pkts,
                      (unsigned long long)st[i].talk_rx_bytes,
                      (unsigned long long)st[i].whip_rx_pkts,
                      (unsigned long long)st[i].whip_rx_bytes,
                      (unsigned long long)st[i].whip_rx_dropped,
                      (unsigned long long)st[i].whip_srtp_fail,
                      (unsigned long long)st[i].created_ms,
                      (unsigned long long)st[i].last_rx_ms) != 0) {
            goto fail;
        }
    }
    if (webapi_jb_append(&jb,
                  "],\"total\":{\"sessions\":%u,\"rtpqBytes\":%llu,\"nackCacheBytes\":%llu,"
                  "\"whipRxBytes\":%llu}}",
                  (unsigned)n, (unsigned long long)total_rtpq_bytes,
                  (unsigned long long)total_nack_bytes,
                  (unsigned long long)total_whip_rx_bytes) != 0) {
        goto fail;
    }
    *http_status = 200;
    free(st);
    return jb.len;

fail:
    *http_status = 500;
    free(st);
    return webapi_respond_json(ZMS_API_OTHER_FAILED, "response too large", NULL, body, body_cap);
}

size_t webapi_handle_webrtc_capabilities(char *body, size_t body_cap, int *http_status)
{
    *http_status = 200;
    return (size_t)snprintf(
        body, body_cap,
        "{\"code\":0,\"data\":{"
        "\"backend\":\"native\",\"ice\":\"lite/full\",\"stun\":true,\"turn\":true,"
        "\"video\":[\"H264\"],\"audio\":[\"Opus\",\"PCMA\",\"PCMU\"],\"directOnly\":true,"
        "\"play\":\"whep\",\"publish\":\"whip\",\"trickle\":true,"
        "\"whepClient\":true,\"whipClient\":true,\"talk\":true,\"iceRestart\":true,"
        "\"turnUdp\":true,\"turnTcp\":false,\"turnTls\":false,"
        "\"nack\":true,\"simulcast\":false,\"transcode\":false,"
        "\"maxSessions\":%u}}",
        (unsigned)zms_webrtc_session_limit());
}

size_t webapi_handle_start_webrtc_client(const zms_webapi_query_map *q, zms_webapi_json_buf *jb,
                                         const zms_config *cfg)
{
    void *ssl_ctx = NULL;

    if (!q || !jb || !q->kind[0] || !q->url[0] || !q->app[0] || !q->stream[0]) {
        return webapi_jb_append(jb, "{\"code\":400,\"msg\":\"kind/url/app/stream required\"}") == 0
                   ? jb->len
                   : 0;
    }
    if (strcmp(q->kind, "whep") != 0 && strcmp(q->kind, "whip") != 0) {
        return webapi_jb_append(jb, "{\"code\":400,\"msg\":\"kind must be whep or whip\"}") == 0 ? jb->len
                                                                                          : 0;
    }
    if (cfg && strncmp(q->url, "https://", 8) == 0) {
        ssl_ctx = zms_pull_ssl_ctx(cfg);
    }
    if (zms_webrtc_client_launch(q->kind, q->url, q->app, q->stream, q->token, ssl_ctx) != 0) {
        return webapi_jb_append(jb, "{\"code\":500,\"msg\":\"launch failed\"}") == 0 ? jb->len : 0;
    }
    if (webapi_jb_append(jb, "{\"code\":0,\"msg\":\"queued\",\"data\":{\"kind\":\"%s\",\"url\":\"%s\","
                      "\"app\":\"%s\",\"stream\":\"%s\"}}",
                  q->kind, q->url, q->app, q->stream) != 0) {
        return 0;
    }
    return jb->len;
}

size_t webapi_handle_get_webrtc_clients(zms_webapi_json_buf *jb)
{
    zms_webrtc_client_stats st[8];
    size_t n = zms_webrtc_client_stats_all(st, sizeof(st) / sizeof(st[0]));
    size_t i;

    if (webapi_jb_append(jb, "{\"code\":0,\"data\":[") != 0) {
        return 0;
    }
    for (i = 0; i < n; ++i) {
        if (webapi_jb_append(jb,
                      "%s{\"id\":\"%s\",\"kind\":\"%s\",\"url\":\"%s\",\"app\":\"%s\","
                      "\"stream\":\"%s\",\"session\":\"%s\",\"state\":%d,"
                      "\"iceConnected\":%d,\"dtlsReady\":%d,\"mediaStarted\":%d,"
                      "\"createdMs\":%llu,\"error\":\"%s\"}",
                      i == 0 ? "" : ",", st[i].id, st[i].kind, st[i].url, st[i].app, st[i].stream,
                      st[i].session_id, st[i].state, st[i].ice_connected, st[i].dtls_ready,
                      st[i].media_started, (unsigned long long)st[i].created_ms,
                      st[i].error) != 0) {
            return 0;
        }
    }
    if (webapi_jb_append(jb, "]}") != 0) {
        return 0;
    }
    return jb->len;
}

size_t webapi_handle_stop_webrtc_client(const zms_webapi_query_map *q, zms_webapi_json_buf *jb)
{
    if (!q || !jb || !q->id[0]) {
        return webapi_jb_append(jb, "{\"code\":400,\"msg\":\"id required\"}") == 0 ? jb->len : 0;
    }
    if (zms_webrtc_client_stop(q->id) != 0) {
        return webapi_jb_append(jb, "{\"code\":404,\"msg\":\"not found\"}") == 0 ? jb->len : 0;
    }
    if (webapi_jb_append(jb, "{\"code\":0,\"msg\":\"stopping\",\"data\":{\"id\":\"%s\"}}", q->id) != 0) {
        return 0;
    }
    return jb->len;
}

#endif /* ZMS_ENABLE_WEBRTC */