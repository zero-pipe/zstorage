/**
 * @file h264_config.c
 * @brief H.264 配置接口实现（封装 zm libflv mpeg4_avc_t）。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/media/codec/h264/h264_config.h"
#include "zm/codec/bitstream.h"
#include <string.h>

typedef char zcs_avc_config_size_check
    [(sizeof(struct mpeg4_avc_t) <= sizeof(((zcs_avc_config *)0)->opaque)) ? 1 : -1];

static struct mpeg4_avc_t *avc_of(zcs_avc_config *c)
{
    return (struct mpeg4_avc_t *)(void *)c->opaque.bytes;
}

static const struct mpeg4_avc_t *avc_const(const zcs_avc_config *c)
{
    return (const struct mpeg4_avc_t *)(const void *)c->opaque.bytes;
}

int zcs_avc_config_load_record(zcs_avc_config *c, const uint8_t *avcc, size_t len)
{
    if (!c || !avcc || len == 0) {
        return -1;
    }
    return mpeg4_avc_decoder_configuration_record_load(avcc, len, avc_of(c));
}

int zcs_avc_config_load_annexb(zcs_avc_config *c, const uint8_t *annexb, size_t len)
{
    if (!c || !annexb || len == 0) {
        return -1;
    }
    return mpeg4_avc_from_nalu(annexb, len, avc_of(c));
}

int zcs_avc_config_nalu_length(const zcs_avc_config *c)
{
    return c ? (int)avc_const(c)->nalu : 0;
}

int zcs_avc_config_mp4_to_annexb(const zcs_avc_config *c, const uint8_t *mp4, size_t len,
                                 uint8_t *out, size_t cap)
{
    if (!c || !mp4 || len == 0 || !out) {
        return -1;
    }
    return h264_mp4toannexb(avc_const(c), mp4, len, out, cap);
}

int zcs_avc_config_annexb_to_mp4(zcs_avc_config *c, const uint8_t *annexb, size_t len, uint8_t *out,
                                 size_t cap, int *vcl, int *update)
{
    int v = 0;
    int u = 0;
    int n;

    if (!c || !annexb || len == 0 || !out) {
        return -1;
    }
    n = h264_annexbtomp4(avc_of(c), annexb, len, out, cap, &v, &u);
    if (vcl) {
        *vcl = v;
    }
    if (update) {
        *update = u;
    }
    return n;
}

int zcs_avc_nalu_to_annexb(int nalu_length, const uint8_t *mp4, size_t len, uint8_t *out,
                           size_t cap)
{
    struct mpeg4_avc_t avc;

    if (!mp4 || len == 0 || !out) {
        return -1;
    }
    memset(&avc, 0, sizeof(avc));
    if (nalu_length > 0) {
        avc.nalu = (uint8_t)nalu_length;
    }
    return h264_mp4toannexb(&avc, mp4, len, out, cap);
}
