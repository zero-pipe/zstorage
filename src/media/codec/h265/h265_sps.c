/**
 * @file h265_sps.c
 * @brief H.265 SPS 解析：只取分辨率相关字段。
 *
 * 参考 HEVC 规范 7.3.2.2 的 SPS 语法，跳过 profile_tier_level，
 * 读取 pic_width/height_in_luma_samples 与 conformance window。
 */
#include "zcs/media/codec/h265/h265_sps.h"
#include <string.h>

typedef struct {
    const uint8_t *buf;
    size_t size;
    size_t bit;
} h265_br;

static int br_eof(const h265_br *b)
{
    return b->bit >= b->size * 8;
}

static uint32_t br_bit(h265_br *b)
{
    size_t byte;
    size_t bit;

    if (br_eof(b)) {
        return 0;
    }
    byte = b->bit / 8;
    bit = 7 - (b->bit % 8);
    b->bit++;
    return (b->buf[byte] >> bit) & 1u;
}

static uint32_t br_read_ue(h265_br *b)
{
    int zeros = 0;
    uint32_t value = 0;

    while (!br_eof(b) && br_bit(b) == 0) {
        ++zeros;
        if (zeros > 31) {
            return 0;
        }
    }
    if (br_eof(b)) {
        return 0;
    }
    if (zeros > 0) {
        value = (1u << zeros) - 1u;
    }
    while (zeros-- > 0) {
        value += br_bit(b) << zeros;
    }
    return value;
}

static size_t rbsp_unescape(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap)
{
    size_t i;
    size_t j = 0;

    for (i = 0; i < in_len && j < cap; ++i) {
        if (i + 2 < in_len && in[i] == 0 && in[i + 1] == 0 && in[i + 2] == 0x03) {
            out[j++] = 0;
            out[j++] = 0;
            i += 2;
        } else {
            out[j++] = in[i];
        }
    }
    return j;
}

#define H265_BIT(ptr, off) (((ptr)[(off) / 8] >> (7 - ((off) % 8))) & 0x01)

static int profile_tier_level_bytes(const uint8_t *p, size_t bytes, uint8_t max_sub_layers_minus1)
{
    uint8_t sub_profile[8];
    uint8_t sub_level[8];
    size_t n;
    uint8_t i;

    if (bytes < 12) {
        return -1;
    }
    if (max_sub_layers_minus1 < 1) {
        return 12;
    }
    if (bytes < 14) {
        return -1;
    }
    for (i = 0; i < max_sub_layers_minus1 && i < 8; ++i) {
        sub_profile[i] = (uint8_t)H265_BIT(p, 12u * 8u + i * 2u);
        sub_level[i] = (uint8_t)H265_BIT(p, 12u * 8u + i * 2u + 1u);
    }
    n = 14;
    for (i = 0; i < max_sub_layers_minus1 && i < 8; ++i) {
        if (sub_profile[i]) {
            n += 11;
        }
        if (sub_level[i]) {
            n += 1;
        }
    }
    return bytes >= n ? (int)n : -1;
}

int zcs_h265_sps_parse(const uint8_t *sps, size_t len, zcs_h265_sps_info *info)
{
    uint8_t rbsp[512];
    size_t n;
    uint8_t max_sub_layers_minus1;
    int ptl_bytes;
    h265_br br;
    uint32_t chroma_format_idc;
    uint32_t width;
    uint32_t height;
    int sub_w = 1;
    int sub_h = 1;

    if (!sps || len < 4 || !info) {
        return 0;
    }
    memset(info, 0, sizeof(*info));
    n = rbsp_unescape(sps, len, rbsp, sizeof(rbsp));
    if (n < 5) {
        return 0;
    }
    /* rbsp[0..1] 为 NAL 头；[2] 起为 SPS RBSP */
    max_sub_layers_minus1 = (uint8_t)((rbsp[2] >> 1) & 0x07);
    ptl_bytes = profile_tier_level_bytes(rbsp + 3, n - 3, max_sub_layers_minus1);
    if (ptl_bytes < 0) {
        return 0;
    }
    br.buf = rbsp;
    br.size = n;
    br.bit = (size_t)(3 + ptl_bytes) * 8u;

    (void)br_read_ue(&br); /* sps_seq_parameter_set_id */
    chroma_format_idc = br_read_ue(&br);
    if (chroma_format_idc == 3) {
        (void)br_bit(&br); /* separate_colour_plane_flag */
    }
    width = br_read_ue(&br);
    height = br_read_ue(&br);
    if (width == 0 || height == 0) {
        return 0;
    }
    if (br_bit(&br)) { /* conformance_window_flag */
        uint32_t left = br_read_ue(&br);
        uint32_t right = br_read_ue(&br);
        uint32_t top = br_read_ue(&br);
        uint32_t bottom = br_read_ue(&br);

        if (chroma_format_idc == 1) {
            sub_w = 2;
            sub_h = 2;
        } else if (chroma_format_idc == 2) {
            sub_w = 2;
            sub_h = 1;
        }
        if ((uint64_t)(left + right) * (uint64_t)sub_w < width) {
            width -= (left + right) * (uint32_t)sub_w;
        }
        if ((uint64_t)(top + bottom) * (uint64_t)sub_h < height) {
            height -= (top + bottom) * (uint32_t)sub_h;
        }
    }
    if (br_eof(&br)) {
        return 0;
    }
    info->width = (int)width;
    info->height = (int)height;
    return 1;
}
