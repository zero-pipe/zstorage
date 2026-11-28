/**
 * @file whip_ingress.c
 * @brief WHIP 推流入站：UDP 接收。
 */
#include "session/webrtc/whip_ingress.h"
#include "bridge/zm_rtp_ingest.h"
#include "bridge/zm_zcs_bridge.h"
#include "session/webrtc/play_session.h"
#include "protocol/webrtc/media_gateway.h"
#include "protocol/webrtc/media_internal.h"
#include "session/webrtc/session_internal.h"
#include "zcs/core/event.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/media/media_limits.h"
#include "zcs/media/wire/rtp_packet.h"
#include "zcs/media/wire/srtp.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

/* 单个 WHIP 会话入站 SRTP 字节速率上限（每秒）。 */
#define ZCS_WHIP_MAX_BYTES_PER_SEC (32u * 1024u * 1024u)

static unsigned g_whip_srtp_ok;
static unsigned g_whip_srtp_fail;
typedef struct zcs_webrtc_whip_ingress_ctx {
    zcs_webrtc_session *session;
    zcs_webrtc_gateway_ingest_cfg gw;
    void *zm_rtp;
} zcs_webrtc_whip_ingress_ctx;

static int whip_zm_on_packet(void *user, const zm_packet *pkt)
{
    zcs_webrtc_session *s = (zcs_webrtc_session *)user;
    zcs_frame frame;

    if (!s || !s->ingest || !pkt) {
        return 0;
    }
    if (zcs_zm_frame_from_packet(pkt, &frame) == ZM_OK) {
        (void)zcs_live_ingest_input_frame(s->ingest, &frame);
    }
    return 0;
}

static void whip_store_h264_nal(zcs_webrtc_session *s, const uint8_t *nal, size_t nlen)
{
    int type;
    if (!s || !nal || nlen == 0) {
        return;
    }
    type = nal[0] & 0x1f;
    if (type == 7 && nlen <= sizeof(s->whip_h264_sps)) {
        memcpy(s->whip_h264_sps, nal, nlen);
        s->whip_h264_sps_len = nlen;
    } else if (type == 8 && nlen <= sizeof(s->whip_h264_pps)) {
        memcpy(s->whip_h264_pps, nal, nlen);
        s->whip_h264_pps_len = nlen;
    }
}

static void whip_try_apply_h264_ps(zcs_webrtc_session *s)
{
    if (!s || !s->ingest) {
        return;
    }
    if (s->source && s->source->video.ready) {
        return;
    }
    if (s->whip_h264_sps_len == 0 || s->whip_h264_pps_len == 0) {
        return;
    }
    if (zcs_live_ingest_set_h264_sps_pps(s->ingest, s->whip_h264_sps, s->whip_h264_sps_len,
                                         s->whip_h264_pps, s->whip_h264_pps_len) == ZN_OK) {
        zn_info("[webrtc] WHIP applied inband H264 SPS=%zu PPS=%zu app=%s stream=%s",
                 s->whip_h264_sps_len, s->whip_h264_pps_len, s->app, s->stream);
    }
}
/** WHIP 客户端常将 SPS/PPS 作为独立单 NAL RTP 包发送（offer 无 sprop）。 */
static void whip_collect_h264_rtp_ps(zcs_webrtc_session *s, const uint8_t *payload, size_t size)
{
    size_t off;
    if (!s || !payload || size < 2 || s->video_codec != ZCS_CODEC_H264) {
        return;
    }
    if (s->source && s->source->video.ready) {
        return;
    }
    off = 0;
    while (off < size) {
        int type = payload[off] & 0x1f;
        if (type >= 1 && type <= 23) {
            whip_store_h264_nal(s, payload + off, size - off);
            break;
        }
        if (type == 24) {
            size_t p = off + 1;
            while (p + 2 <= size) {
                size_t nlen = ((size_t)payload[p] << 8) | payload[p + 1];
                p += 2;
                if (nlen == 0 || p + nlen > size) {
                    break;
                }
                whip_store_h264_nal(s, payload + p, nlen);
                p += nlen;
            }
            break;
        }
        break;
    }
    whip_try_apply_h264_ps(s);
}

static void webrtc_whip_start_media(zcs_webrtc_session *s)
{
    zcs_webrtc_whip_ingress_ctx *ctx;
    uint8_t key[ZCS_WEBRTC_SRTP_KEY_LEN];
    uint8_t salt[ZCS_WEBRTC_SRTP_SALT_LEN];
    if (!s || s->media_started) {
        return;
    }
#if defined(ZCS_HAVE_WEBRTC_DTLS) && ZCS_HAVE_WEBRTC_DTLS
    if (!s->dtls) {
        return;
    }
    if (s->dtls_as_client) {
        /* 主动拉流：远端 DTLS server 用 server_write 发送。 */
        if (zcs_webrtc_dtls_export_server_srtp(s->dtls, key, salt) != 0) {
            return;
        }
    } else if (zcs_webrtc_dtls_export_client_srtp(s->dtls, key, salt) != 0) {
        return;
    }
    if (!s->video_srtp) {
        s->video_srtp = zcs_webrtc_srtp_create();
    }
    if (!s->audio_srtp) {
        s->audio_srtp = zcs_webrtc_srtp_create();
    }
    if (!s->video_srtp || !s->audio_srtp) {
        return;
    }
    zcs_webrtc_srtp_init_recv(s->video_srtp, key, salt);
    zcs_webrtc_srtp_init_recv(s->audio_srtp, key, salt);
    if (zcs_webrtc_session_io_buf_ensure(s) != 0) {
        return;
    }
    zcs_webrtc_srtp_bind_scratch(s->video_srtp, s->io_buf, s->io_cap);
    zcs_webrtc_srtp_bind_scratch(s->audio_srtp, s->io_buf, s->io_cap);
#else
    return;
#endif
    ctx = (zcs_webrtc_whip_ingress_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->session = s;
    zcs_webrtc_gateway_ingest_cfg_defaults(&ctx->gw);
    ctx->gw.answer_has_video = s->answer_has_video;
    ctx->gw.answer_has_audio = s->answer_has_audio;
    ctx->gw.wire_video_pt = s->video_pt;
    ctx->gw.wire_audio_pt = s->audio_pt;
    if (s->ingest) {
        zcs_codec_id ac = s->answer_has_audio ? s->audio_codec : ZCS_CODEC_INVALID;
        uint32_t ahz = s->audio_rate > 0 ? (uint32_t)s->audio_rate : 48000u;
        zcs_live_ingest_set_rtp_clocks(s->ingest, 90000, ac, ahz);
    }
    {
        ctx->zm_rtp = zcs_zm_rtp_ingest_create(2, 0, whip_zm_on_packet, s);
        if (ctx->zm_rtp) {
            if (s->answer_has_video && s->video_codec != ZCS_CODEC_INVALID) {
                (void)zcs_zm_rtp_ingest_track((zcs_zm_rtp_ingest *)ctx->zm_rtp, 0,
                                               s->video_codec, ctx->gw.canon_video_pt, 90000,
                                               NULL);
            }
            if (s->answer_has_audio && s->audio_codec != ZCS_CODEC_INVALID) {
                uint32_t ahz = s->audio_rate > 0 ? (uint32_t)s->audio_rate : 48000u;
                (void)zcs_zm_rtp_ingest_track((zcs_zm_rtp_ingest *)ctx->zm_rtp, 1,
                                               s->audio_codec, ctx->gw.canon_audio_pt, (int)ahz,
                                               NULL);
            }
        }
    }
    s->whip_ingress = ctx;
    s->media_started = 1;
    zcs_webrtc_publish_apply_offer_h264_sprop(s);
    if (s->source && !s->source->publishing) {
        zcs_media_event_publish(s->source, ZCS_ORIGIN_WEBRTC_PUSH);
    }
    zn_info("[webrtc] WHIP publish media ready id=%s video=%s audio=%s app=%s stream=%s", s->id,
             zcs_codec_name(s->video_codec), zcs_codec_name(s->audio_codec), s->app, s->stream);
}

static void webrtc_whip_on_rtp(zcs_webrtc_session *s, uint8_t *rtp, size_t len,
                               struct zcs_webrtc_srtp *srtp, int is_video)
{
    zcs_webrtc_whip_ingress_ctx *ctx;
    zcs_rtp_packet pkt;
    if (!s || !rtp || len < 12 || !srtp) {
        return;
    }
    ctx = (zcs_webrtc_whip_ingress_ctx *)s->whip_ingress;
    if (!ctx) {
        return;
    }
    if (zcs_webrtc_srtp_unprotect(srtp, rtp, &len) != 0) {
        ++g_whip_srtp_fail;
        ++s->rt.whip_srtp_fail;
        if (g_whip_srtp_fail <= 5 || (g_whip_srtp_fail % 500) == 0) {
            zn_warn("[webrtc] SRTP unprotect fail total=%u", g_whip_srtp_fail);
        }
        return;
    }
    ++g_whip_srtp_ok;
    zcs_webrtc_gateway_remap_pt(rtp, len, &ctx->gw, is_video);
    if (zcs_rtp_parse(rtp, len, &pkt) != ZN_OK) {
        ++s->rt.whip_rx_dropped;
        return;
    }
    if (g_whip_srtp_ok <= 8 && pkt.payload && pkt.payload_size > 0) {
        zn_info("[webrtc] SRTP ok #%u seq=%u ext=%u pt=%u pay0=%02x pay1=%02x len=%zu",
                 g_whip_srtp_ok, pkt.hdr.seq, pkt.hdr.extension, pkt.hdr.pt, pkt.payload[0],
                 pkt.payload_size > 1 ? pkt.payload[1] : 0, pkt.payload_size);
    }
    if (is_video && pkt.payload && pkt.payload_size > 0) {
        whip_collect_h264_rtp_ps(s, pkt.payload, pkt.payload_size);
    }
    if (ctx->zm_rtp) {
        (void)zcs_zm_rtp_ingest_input((zcs_zm_rtp_ingest *)ctx->zm_rtp, is_video ? 0 : 1, rtp,
                                       len);
    }
}

static void webrtc_whip_on_srtcp(zcs_webrtc_session *s, const uint8_t *wire, size_t wire_len)
{
    size_t plain_len;
    uint8_t *rtcp_slot;
    if (!s || !wire || wire_len < 8 || !s->io_buf || wire_len > s->io_cap) {
        return;
    }
    rtcp_slot = s->io_buf + ZCS_WEBRTC_PLAY_RTP_SLOT_MAX;
    if (wire_len > s->io_cap - ZCS_WEBRTC_PLAY_RTP_SLOT_MAX) {
        return;
    }
    memcpy(rtcp_slot, wire, wire_len);
    plain_len = wire_len;
    if (zcs_webrtc_srtcp_unprotect(s->video_srtp, rtcp_slot, &plain_len) != 0) {
        if (zcs_webrtc_rtcp_is_psfb_pli(wire, wire_len) && s->ingest) {
            zcs_live_ingest_reset_upstream(s->ingest);
        }
        return;
    }
    if (zcs_webrtc_rtcp_is_psfb_pli(rtcp_slot, plain_len) && s->ingest) {
        zcs_live_ingest_reset_upstream(s->ingest);
    }
}

void zcs_webrtc_whip_ingress_on_udp(zcs_webrtc_session *s, const void *data, size_t len)
{
    const uint8_t *pkt = (const uint8_t *)data;
    zcs_webrtc_gateway_pkt_kind kind;
    zcs_webrtc_whip_ingress_ctx *ctx;
    int track;
    uint8_t pt;
    if (!s || !pkt || len == 0) {
        return;
    }
    kind = zcs_webrtc_gateway_classify(pkt, len);
    if (kind == ZCS_WEBRTC_GATEWAY_STUN || kind == ZCS_WEBRTC_GATEWAY_DTLS) {
        if (!s->dtls_ready) {
            (void)zcs_webrtc_play_on_stun_dtls(s, data, len);
            if (s->dtls_ready) {
                webrtc_whip_start_media(s);
            }
        } else {
            (void)zcs_webrtc_play_on_stun_dtls(s, data, len);
        }
        return;
    }
    if (!s->dtls_ready || !s->media_started) {
        return;
    }
    ctx = (zcs_webrtc_whip_ingress_ctx *)s->whip_ingress;
    if (kind == ZCS_WEBRTC_GATEWAY_SRTCP) {
        if (!s->io_buf || len > s->io_cap) {
            return;
        }
        webrtc_whip_on_srtcp(s, pkt, len);
        return;
    }
    if (kind != ZCS_WEBRTC_GATEWAY_SRTP) {
        return;
    }
    ++s->rt.whip_rx_pkts;
    s->rt.whip_rx_bytes += len;
    {
        uint64_t now = zn_monotonic_ms();
        if (now - s->rt.whip_win_start_ms >= 1000) {
            s->rt.whip_win_start_ms = now;
            s->rt.whip_win_bytes = 0;
        }
        if (s->rt.whip_win_bytes + len > ZCS_WHIP_MAX_BYTES_PER_SEC) {
            ++s->rt.whip_rx_dropped;
            return;
        }
        s->rt.whip_win_bytes += len;
    }
    if (!s->io_buf || len + ZCS_WEBRTC_PLAY_RTP_SLOT_MAX > s->io_cap) {
        ++s->rt.whip_rx_dropped;
        return;
    }
    pt = zcs_rtp_payload_type(pkt, len);
    track = ctx ? zcs_webrtc_gateway_match_media_track(&ctx->gw, pt) : -1;
    if (track < 0) {
        ++s->rt.whip_rx_dropped;
        return;
    }
    {
        uint8_t *rtp_slot = s->io_buf + ZCS_WEBRTC_PLAY_RTP_SLOT_MAX;
        memcpy(rtp_slot, pkt, len);
        webrtc_whip_on_rtp(s, rtp_slot, len, track ? s->video_srtp : s->audio_srtp, track);
    }
}

void zcs_webrtc_whip_ingress_stop(zcs_webrtc_session *s)
{
    zcs_webrtc_whip_ingress_ctx *ctx;
    if (!s) {
        return;
    }
    ctx = (zcs_webrtc_whip_ingress_ctx *)s->whip_ingress;
    if (ctx) {
        if (ctx->zm_rtp) {
            zcs_zm_rtp_ingest_destroy((zcs_zm_rtp_ingest *)ctx->zm_rtp);
        }
        free(ctx);
        s->whip_ingress = NULL;
    }
    zcs_webrtc_play_stop(s);
    if (s->ingest) {
        if (s->source && s->source->publishing) {
            int linger = zcs_media_source_linger(s->source, ZCS_ORIGIN_WEBRTC_PUSH);

            if (!linger) {
                zcs_media_event_publish_fini(s->source, ZCS_ORIGIN_WEBRTC_PUSH);
            }
        }
        zcs_live_ingest_destroy(s->ingest);
        s->ingest = NULL;
        s->source = NULL;
    }
}
