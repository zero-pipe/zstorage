/**
 * @file aac_over_rtp.c
 * @brief AAC ES 校验/遍历与 raw 规范化（RTP 用）。
 */
#include "zcs/media/codec/aac/aac_over_rtp.h"
#include "zcs/media/clock.h"
#include "zm/transport/rtp.h"
#include <stdlib.h>
#include <string.h>

/* --- 无状态 parse --- */

static int adts_sync(const uint8_t *p, size_t left)
{
    return left >= 7 && p[0] == 0xff && (p[1] & 0xf0) == 0xf0;
}

/* protection_absent=1 为 7 字节头；0 则 7 字节 + 2 字节 CRC 后接 raw AU。 */
static size_t adts_hdr_len(const uint8_t *p, size_t left)
{
    if (!adts_sync(p, left)) {
        return 0;
    }
    return (p[1] & 0x01) ? 7u : 9u;
}

static size_t adts_frame_length(const uint8_t *p, size_t left)
{
    size_t flen;

    if (!adts_sync(p, left)) {
        return 0;
    }
    flen = ((size_t)(p[3] & 0x03) << 11) | ((size_t)p[4] << 3) | ((size_t)(p[5] & 0xe0) >> 5);
    if (flen < 7 || flen > 8192) {
        return 0;
    }
    return flen;
}

/** PES ADTS 帧完整覆盖 payload（CC gap 专用，比 foreach 探针更严） */
int zcs_aac_es_strict_valid(const uint8_t *es, size_t len)
{
    size_t off = 0;

    if (!es || len < 7 || !adts_sync(es, len)) {
        return 0;
    }
    while (off + 7 <= len) {
        size_t hdr = adts_hdr_len(es + off, len - off);
        size_t flen = adts_frame_length(es + off, len - off);
        if (hdr < 7 || flen < hdr || off + flen > len) {
            return 0;
        }
        off += flen;
    }
    return off == len;
}

zn_err_t zcs_aac_es_foreach_frame(const uint8_t *es, size_t len, zcs_aac_au_cb cb, void *user)
{
    if (!es || len == 0 || !cb) {
        return ZN_ERR_INVALID;
    }

    if (adts_sync(es, len)) {
        size_t off = 0;
        unsigned frames = 0;
        while (off + 7 <= len) {
            size_t hdr = adts_hdr_len(es + off, len - off);
            size_t flen = adts_frame_length(es + off, len - off);
            if (hdr < 7 || flen < hdr || off + flen > len) {
                break;
            }
            if (cb(es + off + hdr, flen - hdr, user) != 0) {
                break;
            }
            off += flen;
            frames++;
        }
        if (frames > 0) {
            return ZN_OK;
        }
    }

    return cb(es, len, user) == 0 ? ZN_OK : ZN_ERR_INVALID;
}

zn_err_t zcs_aac_es_to_raw(const uint8_t *es, size_t len, const uint8_t **raw, size_t *raw_len)
{
    if (!es || len == 0 || !raw || !raw_len) {
        return ZN_ERR_INVALID;
    }

    if (adts_sync(es, len)) {
        size_t hdr = adts_hdr_len(es, len);
        size_t flen = adts_frame_length(es, len);
        if (hdr >= 7 && flen >= hdr && flen <= len) {
            *raw = es + hdr;
            *raw_len = flen - hdr;
            return ZN_OK;
        }
    }

    *raw = es;
    *raw_len = len;
    return ZN_OK;
}
