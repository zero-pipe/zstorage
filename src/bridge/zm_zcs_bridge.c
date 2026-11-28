/**
 * @file zm_zcs_bridge.c
 * @brief ZM ↔ ZCS 类型桥（zm_mem / codec / track 映射）。
 */
#include "bridge/zm_zcs_bridge.h"
#include "flv-header.h"

#include <string.h>

static void bridge_zn_retain(void *owner)
{
    zn_buf_ref((zn_buf *)owner);
}

static void bridge_zn_release(void *owner)
{
    zn_buf_unref((zn_buf *)owner);
}

zm_mem zcs_zm_mem_from_zn(zn_buf *buf)
{
    if (!buf) {
        return zm_mem_borrow(NULL, 0);
    }
    return zm_mem_wrap(buf, bridge_zn_retain, bridge_zn_release,
                        (void *)(uintptr_t)zn_buf_data(buf), zn_buf_len(buf), zn_buf_cap(buf));
}

zm_codec_id zcs_zm_codec_from_zcs(zcs_codec_id id)
{
    if (id >= ZCS_CODEC_H264 && id <= ZCS_CODEC_H266) {
        return (zm_codec_id)id;
    }
    return ZM_CODEC_INVALID;
}

zcs_codec_id zcs_zm_codec_to_zcs(zm_codec_id id)
{
    if (id >= ZM_CODEC_H264 && id <= ZM_CODEC_H266) {
        return (zcs_codec_id)id;
    }
    return ZCS_CODEC_INVALID;
}

static zm_kind bridge_kind(zcs_track_type track)
{
    switch (track) {
    case ZCS_TRACK_VIDEO:
        return ZM_KIND_VIDEO;
    case ZCS_TRACK_AUDIO:
        return ZM_KIND_AUDIO;
    default:
        return ZM_KIND_UNKNOWN;
    }
}

static zcs_track_type bridge_track_type(zm_kind kind)
{
    switch (kind) {
    case ZM_KIND_VIDEO:
        return ZCS_TRACK_VIDEO;
    case ZM_KIND_AUDIO:
        return ZCS_TRACK_AUDIO;
    default:
        return ZCS_TRACK_INVALID;
    }
}

static uint8_t bridge_es_format(zm_codec_id codec)
{
    switch (codec) {
    case ZM_CODEC_H264:
    case ZM_CODEC_H265:
    case ZM_CODEC_H266:
        return ZM_ES_ANNEXB;
    default:
        return ZM_ES_RAW;
    }
}

static uint16_t bridge_flags(int keyframe, int config, int discardable)
{
    uint16_t flags = 0;
    if (keyframe) {
        flags |= ZM_PKT_KEY;
    }
    if (config) {
        flags |= ZM_PKT_CONFIG;
    }
    if (discardable) {
        flags |= ZM_PKT_DISCARDABLE;
    }
    return flags;
}

void zcs_zm_packet_from_frame(const zcs_frame *frame, zm_packet *out)
{
    if (!out) {
        return;
    }
    zm_packet_init(out);
    if (!frame) {
        return;
    }
    out->mem = zm_mem_borrow(frame->data, frame->size);
    out->pts_ms = (int64_t)frame->pts_ms;
    out->dts_ms = (int64_t)frame->dts_ms;
    out->codec = zcs_zm_codec_from_zcs(frame->codec);
    out->kind = bridge_kind(frame->track);
    out->flags = bridge_flags(frame->keyframe, frame->config_frame, frame->drop_able);
    out->es_format = bridge_es_format(out->codec);
}

int zcs_zm_frame_from_packet(const zm_packet *packet, zcs_frame *out)
{
    if (!packet || !out) {
        return ZM_ERR_INVALID;
    }
    zcs_frame_init(out);
    out->data = packet->mem.data;
    out->size = packet->mem.len;
    out->capacity = packet->mem.cap;
    out->owned = 0;
    out->pts_ms = packet->pts_ms > 0 ? (uint64_t)packet->pts_ms : 0;
    out->dts_ms = packet->dts_ms > 0 ? (uint64_t)packet->dts_ms : 0;
    out->codec = zcs_zm_codec_to_zcs(packet->codec);
    out->track = bridge_track_type(packet->kind);
    out->keyframe = (packet->flags & ZM_PKT_KEY) ? 1 : 0;
    out->config_frame = (packet->flags & ZM_PKT_CONFIG) ? 1 : 0;
    out->drop_able = (packet->flags & ZM_PKT_DISCARDABLE) ? 1 : 0;
    return ZM_OK;
}

void zcs_zm_packet_from_gop_slot(const zcs_gop_slot *slot, zm_packet *out)
{
    if (!out) {
        return;
    }
    zm_packet_init(out);
    if (!slot) {
        return;
    }
    out->mem = zm_mem_borrow(slot->data, slot->len);
    out->dts_ms = (int64_t)slot->dts_ms;
    out->pts_ms = slot->pts_ms ? (int64_t)slot->pts_ms : (int64_t)slot->dts_ms;
    out->codec = zcs_zm_codec_from_zcs(slot->codec);
    out->kind = bridge_kind(slot->track);
    out->flags = bridge_flags(slot->keyframe, slot->config_frame, slot->drop_able);
    out->es_format = bridge_es_format(out->codec);
}

void zcs_zm_track_from_media_track(const zcs_media_track *in, zm_track *out)
{
    if (!out) {
        return;
    }
    zm_track_init(out);
    if (!in) {
        return;
    }
    out->codec = zcs_zm_codec_from_zcs(in->codec);
    out->kind = bridge_kind(in->type);
    out->clock_hz = out->kind == ZM_KIND_VIDEO ? 90000 : in->sample_rate;
    out->sample_rate = in->sample_rate;
    out->channels = in->channels;
    out->timebase.num = 1;
    out->timebase.den = 1000;
}

int zcs_zm_tracks_from_source(const zcs_media_source *src, zm_track *tracks, size_t max_tracks,
                               size_t *count)
{
    uint8_t vbuf[4096];
    uint8_t abuf[512];
    size_t vlen = 0;
    size_t alen = 0;
    size_t n = 0;

    if (!src || !tracks || max_tracks == 0 || !count) {
        return ZM_ERR_INVALID;
    }
    if (src->gop_queue) {
        vlen = zcs_gop_queue_copy_video_config(src->gop_queue, vbuf, sizeof(vbuf));
        alen = zcs_gop_queue_copy_audio_config(src->gop_queue, abuf, sizeof(abuf));
    }

    if (src->has_video && src->video.codec != ZCS_CODEC_INVALID && n < max_tracks) {
        zm_track *t = &tracks[n];
        zm_track_init(t);
        t->track_id = 0;
        t->codec = zcs_zm_codec_from_zcs(src->video.codec);
        t->kind = ZM_KIND_VIDEO;
        t->clock_hz = 90000;
        t->width = src->video.width;
        t->height = src->video.height;
        t->timebase.num = 1;
        t->timebase.den = 1000;
        if (vlen > 0) {
            if (t->codec == ZM_CODEC_H264) {
                struct flv_video_tag_header_t vh;
                int h = flv_video_tag_header_read(&vh, vbuf, vlen);
                if (h > 0 && (size_t)h < vlen) {
                    zm_track_set_config(t, ZM_CONFIG_AVCC, vbuf + h, vlen - (size_t)h);
                }
            } else if (t->codec == ZM_CODEC_H265) {
                zm_track_set_config(t, ZM_CONFIG_HVCC, vbuf, vlen);
            } else if (t->codec == ZM_CODEC_H266) {
                zm_track_set_config(t, ZM_CONFIG_VVCC, vbuf, vlen);
            } else if (t->codec == ZM_CODEC_AV1) {
                struct flv_video_tag_header_t vh;
                int h = flv_video_tag_header_read(&vh, vbuf, vlen);
                if (h > 0 && (size_t)h < vlen) {
                    zm_track_set_config(t, ZM_CONFIG_AV1C, vbuf + h, vlen - (size_t)h);
                }
            }
        }
        ++n;
    }

    if (src->has_audio && src->audio.codec != ZCS_CODEC_INVALID && n < max_tracks) {
        zm_track *t = &tracks[n];
        zm_track_init(t);
        t->track_id = 1;
        t->codec = zcs_zm_codec_from_zcs(src->audio.codec);
        t->kind = ZM_KIND_AUDIO;
        t->sample_rate = src->audio.sample_rate;
        t->channels = src->audio.channels;
        t->clock_hz = src->audio.sample_rate > 0 ? src->audio.sample_rate
                                                : zm_codec_clock_hz(t->codec);
        t->timebase.num = 1;
        t->timebase.den = 1000;
        if (alen > 0) {
            struct flv_audio_tag_header_t ah;
            int h = flv_audio_tag_header_read(&ah, abuf, alen);
            if (h > 0 && (size_t)h < alen) {
                zm_track_set_config(t, t->codec == ZM_CODEC_OPUS ? ZM_CONFIG_OPUS_HEAD
                                                                    : ZM_CONFIG_ASC,
                                     abuf + h, alen - (size_t)h);
            }
        }
        ++n;
    }

    *count = n;
    return n ? ZM_OK : ZM_ERR_NOT_SUPPORTED;
}
