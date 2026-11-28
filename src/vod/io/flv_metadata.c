/**
 * @file flv_metadata.c
 * @brief VOD FLV onMetaData 编码。
 */
#include "zcs/vod/io/flv_metadata.h"
#include "zcs/media/container/flv/flv_index.h"
#include "zcs/vod/io/source.h"
#include "zcs/core/stream/hub.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/wire/amf.h"
#include "zcs/media/wire/rtmp.h"
#include <stdlib.h>
#include <string.h>

static size_t amf_encode_key(uint8_t *out, size_t cap, const char *key)
{
    size_t klen = key ? strlen(key) : 0;

    if (cap < 2 + klen) {
        return 0;
    }
    out[0] = (uint8_t)((klen >> 8) & 0xff);
    out[1] = (uint8_t)(klen & 0xff);
    if (klen) {
        memcpy(out + 2, key, klen);
    }
    return 2 + klen;
}

static size_t amf_encode_bool(uint8_t *out, size_t cap, int v)
{
    if (cap < 2) {
        return 0;
    }
    out[0] = ZCS_AMF_BOOLEAN;
    out[1] = v ? 1 : 0;
    return 2;
}

size_t zcs_vod_flv_metadata_encode(const zcs_media_source *src, uint8_t *amf, size_t cap,
                                   double duration_override_sec, const zcs_vod_flv_index *idx,
                                   const zcs_vod_flv_index_view *view)
{
    double width = 0.0, height = 0.0, fps = 0.0;
    double video_codec = 7.0, audio_codec = 10.0;
    double audio_rate = 44100.0;
    double stereo = 1.0;
    double duration_sec = duration_override_sec;
    zcs_codec_id vc = ZCS_CODEC_INVALID;
    const double *kf_times = NULL;
    const double *kf_pos = NULL;
    size_t kf_count = 0;
    double kf_filesize = 0.0;
    size_t pos = 0;

    if (!amf || cap == 0) {
        return 0;
    }

    if (view && view->count > 0) {
        kf_times = view->times;
        kf_pos = view->filepositions;
        kf_count = view->count;
        kf_filesize = view->filesize;
    } else if (idx && idx->count > 0) {
        kf_times = idx->times;
        kf_pos = idx->filepositions;
        kf_count = idx->count;
        kf_filesize = idx->filesize;
    }

    if (src) {
        if (duration_sec <= 0.0 && zcs_media_source_is_vod(src)) {
            duration_sec = zcs_vod_source_duration_ms(src) / 1000.0;
            if (duration_sec <= 0.0) {
                duration_sec = zcs_vod_probe_duration_ms(src->app, src->stream) / 1000.0;
            }
        }
        if (src->video.ready) {
            width = (double)src->video.width;
            height = (double)src->video.height;
            fps = (double)src->video.fps;
            vc = src->video.codec;
        }
        if (vc == ZCS_CODEC_INVALID && src->gop_queue) {
            size_t vlen = 0;
            const uint8_t *vcfg = zcs_media_source_video_config(src, &vlen);
            if (vcfg && vlen) {
                vc = zcs_flv_tag_video_codec(vcfg, vlen);
            }
        }
        video_codec = zcs_flv_metadata_videocodecid(vc != ZCS_CODEC_INVALID ? vc : ZCS_CODEC_H264);
        if (src->audio.ready) {
            audio_rate = (double)src->audio.sample_rate;
            stereo = src->audio.channels > 1 ? 1.0 : 0.0;
        }
    }

    pos += zcs_amf_encode_string(amf + pos, cap - pos, "onMetaData");
    amf[pos++] = ZCS_AMF_OBJECT;
    pos += amf_encode_key(amf + pos, cap - pos, "duration");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, duration_sec);
    if (duration_sec > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "canSeekToEnd");
        pos += amf_encode_bool(amf + pos, cap - pos, 1);
    }
    if (kf_filesize > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "filesize");
        pos += zcs_amf_encode_number(amf + pos, cap - pos, kf_filesize);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "width");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, width);
    pos += amf_encode_key(amf + pos, cap - pos, "height");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, height);
    if (fps > 0.0) {
        pos += amf_encode_key(amf + pos, cap - pos, "framerate");
        pos += zcs_amf_encode_number(amf + pos, cap - pos, fps);
    }
    pos += amf_encode_key(amf + pos, cap - pos, "videocodecid");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, video_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiocodecid");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, audio_codec);
    pos += amf_encode_key(amf + pos, cap - pos, "audiosamplerate");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, audio_rate);
    pos += amf_encode_key(amf + pos, cap - pos, "stereo");
    pos += zcs_amf_encode_number(amf + pos, cap - pos, stereo);
    if (kf_count > 0) {
        pos += amf_encode_key(amf + pos, cap - pos, "keyframes");
        if (pos >= cap) {
            return pos;
        }
        amf[pos++] = ZCS_AMF_OBJECT;
        pos += amf_encode_key(amf + pos, cap - pos, "filepositions");
        pos += zcs_amf_encode_strict_array_numbers(amf + pos, cap - pos, kf_pos, kf_count);
        pos += amf_encode_key(amf + pos, cap - pos, "times");
        pos += zcs_amf_encode_strict_array_numbers(amf + pos, cap - pos, kf_times, kf_count);
        pos += zcs_amf_encode_object_end(amf + pos, cap - pos);
    }
    pos += zcs_amf_encode_object_end(amf + pos, cap - pos);
    return pos;
}

size_t zcs_vod_flv_metadata_body_size(const zcs_media_source *src, double duration_sec,
                                      const zcs_vod_flv_index *idx)
{
    /* 容量随关键帧数增长：堆上算，避免按大 cap 写满 16KB 栈数组。 */
    size_t cap = 16384 + (idx ? idx->count : 0) * 24;
    uint8_t *buf = (uint8_t *)malloc(cap);
    size_t need;

    if (!buf) {
        return 0;
    }
    need = zcs_vod_flv_metadata_encode(src, buf, cap, duration_sec, idx, NULL);
    if (need == 0 || need > cap) {
        free(buf);
        cap = (need > cap ? need : cap) + 256;
        buf = (uint8_t *)malloc(cap);
        if (!buf) {
            return 0;
        }
        need = zcs_vod_flv_metadata_encode(src, buf, cap, duration_sec, idx, NULL);
    }
    free(buf);
    return need;
}
