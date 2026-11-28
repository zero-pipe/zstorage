/**
 * @file frame.c
 * @brief zcs_frame 生命周期与 GOP/同步判定辅助。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/frame.h"
#include "zcs/core/gop/queue.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/codec/frame_policy/frame_codec_policy.h"
#include <stdlib.h>
#include <string.h>

void zcs_frame_init(zcs_frame *f)
{
    if (!f) {
        return;
    }
    memset(f, 0, sizeof(*f));
}

void zcs_frame_clear(zcs_frame *f)
{
    if (!f) {
        return;
    }
    if (f->owned && f->data) {
        free(f->data);
    }
    zcs_frame_init(f);
}

zn_err_t zcs_frame_reserve(zcs_frame *f, size_t cap)
{
    if (!f) {
        return ZN_ERR_INVALID;
    }
    if (cap <= f->capacity) {
        return ZN_OK;
    }
    uint8_t *p = (uint8_t *)realloc(f->owned ? f->data : NULL, cap);
    if (!p) {
        return ZN_ERR_NOMEM;
    }
    f->data = p;
    f->capacity = cap;
    f->owned = 1;
    return ZN_OK;
}

void zcs_frame_refresh_key_from_es(zcs_frame *f)
{
    const zcs_frame_codec_ops *ops;

    if (!f || f->track != ZCS_TRACK_VIDEO || f->config_frame || !f->data || f->size == 0) {
        return;
    }
    ops = zcs_frame_codec_find(f->codec);
    if (ops && ops->is_keyframe && ops->is_keyframe(f)) {
        f->keyframe = 1;
    }
}

int zcs_frame_video_gop_marker(const zcs_frame *f)
{
    const zcs_frame_codec_ops *ops;
    if (!f || f->track != ZCS_TRACK_VIDEO) {
        return 0;
    }
    ops = zcs_frame_codec_find(f->codec);
    if (ops && ops->is_keyframe && ops->is_config_frame) {
        return ops->is_keyframe(f) || ops->is_config_frame(f);
    }
    return f->keyframe || f->config_frame;
}

int zcs_gop_queue_storable(const zcs_frame *f, int cache_started, int has_video)
{
    const zcs_frame_codec_ops *ops;
    if (!f) {
        return 0;
    }
    ops = zcs_frame_codec_find(f->codec);
    if (ops && ops->gop_queue_storable) {
        return ops->gop_queue_storable(f, cache_started, has_video);
    }
    if (f->track == ZCS_TRACK_AUDIO) {
        return has_video ? cache_started : 1;
    }
    if (f->drop_able) {
        return cache_started;
    }
    if (zcs_frame_video_gop_marker(f)) {
        return 1;
    }
    return cache_started;
}

int zcs_gop_queue_new_gop(const zcs_frame *f, int video_key_pos, int has_video)
{
    const zcs_frame_codec_ops *ops;
    if (!f) {
        return 0;
    }
    ops = zcs_frame_codec_find(f->codec);
    if (ops && ops->gop_queue_new_gop) {
        return ops->gop_queue_new_gop(f, video_key_pos, has_video);
    }
    if (f->track == ZCS_TRACK_AUDIO) {
        return has_video ? 0 : 1;
    }
    (void)video_key_pos;
    if (f->config_frame) {
        return 0;
    }
    return zcs_frame_video_gop_marker(f);
}

int zcs_frame_is_egress_sync(const zcs_frame *f)
{
    const zcs_frame_codec_ops *ops;
    if (!f || f->track != ZCS_TRACK_VIDEO || f->config_frame || !f->data || f->size == 0) {
        return 0;
    }
    if (f->keyframe) {
        return 1;
    }
    ops = zcs_frame_codec_find(f->codec);
    return ops && ops->is_keyframe && ops->is_keyframe(f);
}

int zcs_frame_is_decode_start(const zcs_frame *f)
{
    if (!f || f->track != ZCS_TRACK_VIDEO || f->config_frame || !f->data || f->size == 0) {
        return 0;
    }
    if (f->codec == ZCS_CODEC_H265) {
        /* 接受 IDR 与 CRA（type 16-21）：libx265/FFmpeg 常发 CRA_NUT */
        return zcs_h265_annexb_is_sync_key(f->data, f->size);
    }
    if (f->codec == ZCS_CODEC_H264) {
        return f->keyframe || zcs_h264_annexb_is_sync_key(f->data, f->size);
    }
    return f->keyframe || zcs_frame_is_egress_sync(f);
}

int zcs_frame_is_play_start(const zcs_frame *f)
{
    return zcs_frame_is_decode_start(f);
}

void zcs_gop_queue_timeline_from_frame(const zcs_frame *f, uint32_t *dts_ms, uint32_t *pts_ms)
{
    uint32_t dts;
    uint32_t pts;

    if (!dts_ms || !pts_ms) {
        return;
    }
    if (!f) {
        *dts_ms = 0;
        *pts_ms = 0;
        return;
    }
    dts = (uint32_t)f->dts_ms;
    pts = (uint32_t)f->pts_ms;
    if (pts == 0) {
        pts = dts;
    }
    *dts_ms = dts;
    *pts_ms = (pts != dts) ? pts : 0;
}

uint32_t zcs_gop_slot_dts_ms(const zcs_gop_slot *slot)
{
    return slot ? slot->dts_ms : 0;
}

uint32_t zcs_gop_slot_pts_ms(const zcs_gop_slot *slot)
{
    if (!slot) {
        return 0;
    }
    return slot->pts_ms ? slot->pts_ms : slot->dts_ms;
}

int zcs_gop_slot_is_egress_sync(const zcs_gop_slot *slot)
{
    zcs_frame f;
    if (!slot) {
        return 0;
    }
    zcs_frame_init(&f);
    f.data = slot->data;
    f.size = slot->len;
    f.codec = slot->codec;
    f.track = slot->track;
    f.keyframe = slot->keyframe;
    f.config_frame = slot->config_frame;
    return zcs_frame_is_egress_sync(&f);
}

void zcs_gop_slot_refresh_sync_key(zcs_gop_slot *slot)
{
    if (slot && zcs_gop_slot_is_egress_sync(slot)) {
        slot->keyframe = 1;
    }
}

int zcs_gop_slot_is_decode_start(const zcs_gop_slot *slot)
{
    zcs_frame f;
    if (!slot) {
        return 0;
    }
    zcs_frame_init(&f);
    f.data = slot->data;
    f.size = slot->len;
    f.codec = slot->codec;
    f.track = slot->track;
    f.keyframe = slot->keyframe;
    f.config_frame = slot->config_frame;
    return zcs_frame_is_decode_start(&f);
}

void zcs_gop_slot_refresh_decode_key(zcs_gop_slot *slot)
{
    if (slot && zcs_gop_slot_is_decode_start(slot)) {
        slot->keyframe = 1;
    }
}

int zcs_gop_slot_is_play_start(const zcs_gop_slot *slot)
{
    zcs_frame f;
    if (!slot) {
        return 0;
    }
    zcs_frame_init(&f);
    f.data = slot->data;
    f.size = slot->len;
    f.codec = slot->codec;
    f.track = slot->track;
    f.keyframe = slot->keyframe;
    f.config_frame = slot->config_frame;
    return zcs_frame_is_play_start(&f);
}

void zcs_gop_slot_refresh_play_key(zcs_gop_slot *slot)
{
    if (slot && zcs_gop_slot_is_play_start(slot)) {
        slot->keyframe = 1;
    }
}

zn_err_t zcs_frame_assign(zcs_frame *f, const void *data, size_t len, int copy)
{
    if (!f || (!data && len)) {
        return ZN_ERR_INVALID;
    }
    if (copy) {
        zn_err_t err = zcs_frame_reserve(f, len);
        if (err != ZN_OK) {
            return err;
        }
        memcpy(f->data, data, len);
        f->size = len;
        f->owned = 1;
    } else {
        zcs_frame_clear(f);
        f->data = (uint8_t *)(uintptr_t)data;
        f->size = len;
        f->owned = 0;
    }
    return ZN_OK;
}
