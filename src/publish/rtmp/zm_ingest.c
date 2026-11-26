/**
 * @file zm_ingest.c
 * @brief RTMP tag → zm FLV 入站适配。
 */
#include "publish/rtmp/zm_ingest.h"

#include "bridge/zm_zms_bridge.h"
#include "zm/format/flv.h"

#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"

#include <stdlib.h>
#include <string.h>

struct zms_rtmp_zm_ingest {
    zms_live_ingest *ingest;
    zm_reader *reader;
};

static void rtmp_zm_apply_aac_config(zms_rtmp_zm_ingest *z, const uint8_t *asc, size_t len)
{
    char hex[2 * 64 + 1];
    static const char digits[] = "0123456789abcdef";
    size_t i;

    if (!z || !asc || len == 0 || len > 64) {
        return;
    }
    for (i = 0; i < len; ++i) {
        hex[i * 2] = digits[asc[i] >> 4];
        hex[i * 2 + 1] = digits[asc[i] & 0x0f];
    }
    hex[len * 2] = '\0';
    (void)zms_live_ingest_set_aac_config_hex(z->ingest, hex);
    zms_live_ingest_mark_real_audio(z->ingest);
}

static void rtmp_zm_apply_h264_config(zms_rtmp_zm_ingest *z, const uint8_t *avcc, size_t len)
{
    struct mpeg4_avc_t avc;

    memset(&avc, 0, sizeof(avc));
    if (mpeg4_avc_decoder_configuration_record_load(avcc, len, &avc) <= 0 || avc.nb_sps == 0 ||
        avc.nb_pps == 0) {
        return;
    }
    (void)zms_live_ingest_set_h264_sps_pps(z->ingest, avc.sps[0].data, avc.sps[0].bytes,
                                           avc.pps[0].data, avc.pps[0].bytes);
}

static void rtmp_zm_apply_h265_config(zms_rtmp_zm_ingest *z, const uint8_t *hvcc, size_t len)
{
    struct mpeg4_hevc_t hevc;
    const uint8_t *vps = NULL;
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    size_t vps_len = 0;
    size_t sps_len = 0;
    size_t pps_len = 0;
    int i;

    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(hvcc, len, &hevc) <= 0) {
        return;
    }
    for (i = 0; i < (int)hevc.numOfArrays; ++i) {
        if (hevc.nalu[i].type == 32) {
            vps = hevc.nalu[i].data;
            vps_len = hevc.nalu[i].bytes;
        } else if (hevc.nalu[i].type == 33) {
            sps = hevc.nalu[i].data;
            sps_len = hevc.nalu[i].bytes;
        } else if (hevc.nalu[i].type == 34) {
            pps = hevc.nalu[i].data;
            pps_len = hevc.nalu[i].bytes;
        }
    }
    if (sps && pps) {
        (void)zms_live_ingest_set_h265_vps_sps_pps(z->ingest, vps, vps_len, sps, sps_len, pps,
                                                   pps_len);
    }
}

static void rtmp_zm_apply_config(zms_rtmp_zm_ingest *z, const zm_packet *pkt)
{
    if (!z || !pkt || !pkt->mem.data) {
        return;
    }
    switch (pkt->codec) {
    case ZM_CODEC_H264:
        rtmp_zm_apply_h264_config(z, pkt->mem.data, pkt->mem.len);
        break;
    case ZM_CODEC_H265:
        rtmp_zm_apply_h265_config(z, pkt->mem.data, pkt->mem.len);
        break;
    case ZM_CODEC_AAC:
        rtmp_zm_apply_aac_config(z, pkt->mem.data, pkt->mem.len);
        break;
    case ZM_CODEC_OPUS:
        zms_live_ingest_set_audio_codec(z->ingest, ZMS_CODEC_OPUS, 48000);
        break;
    default:
        /* AV1/G711 等：媒体帧自身携带/推导配置，无需显式 config。 */
        break;
    }
}

static void rtmp_zm_feed_media(zms_rtmp_zm_ingest *z, const zm_packet *pkt)
{
    zms_frame frame;

    if (zms_zm_frame_from_packet(pkt, &frame) != ZM_OK) {
        return;
    }
    (void)zms_live_ingest_input_frame(z->ingest, &frame);
}

zms_rtmp_zm_ingest *zms_rtmp_zm_ingest_create(zms_live_ingest *ingest)
{
    zms_rtmp_zm_ingest *z;
    zm_reader_cfg cfg;

    if (!ingest) {
        return NULL;
    }
    z = (zms_rtmp_zm_ingest *)calloc(1, sizeof(*z));
    if (!z) {
        return NULL;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.format = ZM_FMT_FLV;
    cfg.mode = ZM_READ_ES;
    cfg.input_tag_body = 1;
    z->reader = zm_flv_reader_open_push(&cfg);
    if (!z->reader) {
        free(z);
        return NULL;
    }
    z->ingest = ingest;
    return z;
}

void zms_rtmp_zm_ingest_destroy(zms_rtmp_zm_ingest *z)
{
    if (!z) {
        return;
    }
    if (z->reader) {
        zm_reader_close(z->reader);
    }
    free(z);
}

zn_err_t zms_rtmp_zm_ingest_input_tag(zms_rtmp_zm_ingest *z, int type, const void *body,
                                        size_t len, uint32_t dts_ms)
{
    zm_packet pkt;
    int rc;

    if (!z || !z->reader || !body || len == 0) {
        return ZN_ERR_INVALID;
    }
    if (zm_flv_reader_input_tag(z->reader, type, body, len, dts_ms) != ZM_OK) {
        return ZN_ERR_INVALID;
    }
    while ((rc = zm_reader_read(z->reader, &pkt)) == 1) {
        if (pkt.flags & ZM_PKT_CONFIG) {
            rtmp_zm_apply_config(z, &pkt);
        } else {
            rtmp_zm_feed_media(z, &pkt);
        }
        zm_packet_clear(&pkt);
    }
    return rc < 0 ? ZN_ERR_INVALID : ZN_OK;
}
