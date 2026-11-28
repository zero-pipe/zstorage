/**
 * @file stream_track.c
 * @brief 视频/音频轨清空与尺寸合理性检查。
 */
#include "zcs/media/media_limits.h"
#include "zcs/core/stream_track.h"
#include "zcs/media/codec/h264/h264_sps.h"
#include "zcs/media/codec/h264/h264_over_rtmp.h"
#include "zcs/media/codec/aac/aac_over_rtmp.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <string.h>

void zcs_video_track_clear(zcs_video_track *t)
{
    if (!t) {
        return;
    }
    memset(t, 0, sizeof(*t));
}

void zcs_audio_track_clear(zcs_audio_track *t)
{
    if (!t) {
        return;
    }
    memset(t, 0, sizeof(*t));
}

int zcs_video_size_is_suspicious(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) {
        return 1;
    }
    /* 仅识别常见占位量级，不作为业务分辨率下限 */
    return width < 320u || height < 240u;
}

int zcs_video_size_should_replace(uint32_t cur_w, uint32_t cur_h, uint32_t new_w, uint32_t new_h)
{
    if (new_w == 0 || new_h == 0) {
        return 0;
    }
    if (cur_w == new_w && cur_h == new_h) {
        return 0;
    }
    if (cur_w == 0 || cur_h == 0) {
        return 1;
    }
    /* 已有可信尺寸时，拒绝被占位 SPS 回退；其余以新 SPS 为准 */
    if (!zcs_video_size_is_suspicious(cur_w, cur_h) &&
        zcs_video_size_is_suspicious(new_w, new_h)) {
        return 0;
    }
    return 1;
}

zn_err_t zcs_video_track_from_avc(zcs_video_track *t, const uint8_t *data, size_t len)
{
    if (!t || !data || len < 5) {
        return ZN_ERR_INVALID;
    }

    const uint8_t *sps = NULL, *pps = NULL;
    size_t sps_len = 0, pps_len = 0;
    if (!zcs_rtmp_avc_extract_sps_pps(data, len, &sps, &sps_len, &pps, &pps_len)) {
        return ZN_ERR_INVALID;
    }

    zcs_video_track_clear(t);
    t->codec = ZCS_CODEC_H264;
    if (!zcs_rtmp_avc_profile_level_id(data, len, t->profile_level_id)) {
        snprintf(t->profile_level_id, sizeof(t->profile_level_id), "42001f");
    }

    zcs_h264_sps_info sps_info;
    if (zcs_h264_sps_parse(sps, sps_len, &sps_info)) {
        t->width = sps_info.width;
        t->height = sps_info.height;
        t->fps = sps_info.fps;
    }
    t->ready = 1;
    if (t->width >= ZCS_VIDEO_WIDTH_MIN_VALID && t->height > 0) {
        zn_info("track video: %dx%d fps=%.2f profile=%s", t->width, t->height, (double)t->fps,
                 t->profile_level_id);
    }
    return ZN_OK;
}

zn_err_t zcs_audio_track_from_asc(zcs_audio_track *t, const uint8_t *asc, size_t len)
{
    if (!t || !asc || len == 0) {
        return ZN_ERR_INVALID;
    }

    zcs_audio_track_clear(t);
    t->codec = ZCS_CODEC_AAC;
    if (!zcs_aac_parse_asc(asc, len, &t->sample_rate, &t->channels)) {
        t->sample_rate = 44100;
        t->channels = 2;
    }

    char *p = t->asc_hex;
    for (size_t i = 0; i < len && (size_t)(p - t->asc_hex) < sizeof(t->asc_hex) - 3; ++i) {
        p += snprintf(p, (size_t)(t->asc_hex + sizeof(t->asc_hex) - p), "%02x", asc[i]);
    }
    t->ready = 1;
    zn_info("track audio: codec=%d rate=%d ch=%d asc=%s", (int)t->codec, t->sample_rate,
             t->channels, t->asc_hex);
    return ZN_OK;
}

zn_err_t zcs_audio_track_from_g711(zcs_audio_track *t, zcs_codec_id codec, int sample_rate,
                                    int channels)
{
    if (!t || (codec != ZCS_CODEC_G711A && codec != ZCS_CODEC_G711U)) {
        return ZN_ERR_INVALID;
    }
    zcs_audio_track_clear(t);
    t->codec = codec;
    t->sample_rate = sample_rate > 0 ? sample_rate : 8000;
    t->channels = channels > 0 ? channels : 1;
    t->ready = 1;
    zn_info("track audio: %s rate=%d ch=%d", zcs_codec_name(codec), t->sample_rate, t->channels);
    return ZN_OK;
}

zn_err_t zcs_audio_track_from_rtmp(zcs_audio_track *t, const uint8_t *data, size_t len)
{
    if (!t || !data || len < 2) {
        return ZN_ERR_INVALID;
    }

    zcs_codec_id codec = zcs_flv_tag_audio_codec(data, len);
    if (codec == ZCS_CODEC_G711A || codec == ZCS_CODEC_G711U) {
        return zcs_audio_track_from_g711(t, codec, 8000, 1);
    }
    if (codec != ZCS_CODEC_AAC || len < 4) {
        return ZN_ERR_INVALID;
    }
    return zcs_audio_track_from_asc(t, data + 2, len - 2);
}
