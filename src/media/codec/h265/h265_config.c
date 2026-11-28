/**
 * @file h265_config.c
 * @brief H.265 配置接口实现（封装 zm libflv mpeg4_hevc_t）。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/media/codec/h265/h265_config.h"
#include "zm/codec/bitstream.h"
#include <string.h>

typedef char zcs_hevc_config_size_check
    [(sizeof(struct mpeg4_hevc_t) <= sizeof(((zcs_hevc_config *)0)->opaque)) ? 1 : -1];

static struct mpeg4_hevc_t *hevc_of(zcs_hevc_config *c)
{
    return (struct mpeg4_hevc_t *)(void *)c->opaque.bytes;
}

static const struct mpeg4_hevc_t *hevc_const(const zcs_hevc_config *c)
{
    return (const struct mpeg4_hevc_t *)(const void *)c->opaque.bytes;
}

int zcs_hevc_config_load_record(zcs_hevc_config *c, const uint8_t *hvcc, size_t len)
{
    if (!c || !hvcc || len == 0) {
        return -1;
    }
    return mpeg4_hevc_decoder_configuration_record_load(hvcc, len, hevc_of(c));
}

int zcs_hevc_config_length_size(const zcs_hevc_config *c)
{
    return c ? (int)hevc_const(c)->lengthSizeMinusOne + 1 : 0;
}

int zcs_hevc_config_mp4_to_annexb(const zcs_hevc_config *c, const uint8_t *mp4, size_t len,
                                  uint8_t *out, size_t cap)
{
    if (!c || !mp4 || len == 0 || !out) {
        return -1;
    }
    return h265_mp4toannexb(hevc_const(c), mp4, len, out, cap);
}

int zcs_hevc_config_annexb_to_mp4(zcs_hevc_config *c, const uint8_t *annexb, size_t len,
                                  uint8_t *out, size_t cap, int *vcl, int *update)
{
    int v = 0;
    int u = 0;
    int n;

    if (!c || !annexb || len == 0 || !out) {
        return -1;
    }
    n = h265_annexbtomp4(hevc_of(c), annexb, len, out, cap, &v, &u);
    if (vcl) {
        *vcl = v;
    }
    if (update) {
        *update = u;
    }
    return n;
}

int zcs_hevc_nalu_to_annexb(int length_size, const uint8_t *mp4, size_t len, uint8_t *out,
                            size_t cap)
{
    struct mpeg4_hevc_t hevc;

    if (!mp4 || len == 0 || !out) {
        return -1;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (length_size > 0) {
        hevc.lengthSizeMinusOne = (uint8_t)(length_size - 1);
    }
    return h265_mp4toannexb(&hevc, mp4, len, out, cap);
}
