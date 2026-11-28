/**
 * @file interleaved.c
 * @brief RTSP interleaved（`$`）帧解析：channel + 长度 + RTP 载荷。
 *
 * 帧格式（RFC 2326 §10.12 Embedded Binary Data）：
 *   '$' <channel:1> <length:2 big-endian> <payload...>
 * 本文件拥有该帧格式，不依赖任何 librtsp/librtp 内部实现。
 */
#include "zcs/media/container/container_registry.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum rtsp_ic_state {
    RTSP_IC_START = 0,
    RTSP_IC_CHANNEL,
    RTSP_IC_LEN_HI,
    RTSP_IC_LEN_LO,
    RTSP_IC_DATA,
} rtsp_ic_state;

typedef struct {
    zcs_container_demux_opts cfg;
    rtsp_ic_state state;
    uint8_t channel;
    uint16_t length;
    uint16_t bytes;
    uint16_t capacity;
    uint8_t *data;
} rtsp_ic_ctx;

static void rtsp_ic_onrtp(void *param, uint8_t channel, const void *data, uint16_t bytes)
{
    rtsp_ic_ctx *c = (rtsp_ic_ctx *)param;
    zcs_container_packet pkt;

    if (!c || !c->cfg.on_packet || !data || bytes == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZCS_CONTAINER_PKT_RTP;
    pkt.channel = channel;
    pkt.data = (const uint8_t *)data;
    pkt.len = bytes;
    c->cfg.on_packet(&pkt, c->cfg.user);
}

static int rtsp_ic_alloc(rtsp_ic_ctx *c)
{
    void *p;

    if (c->capacity >= c->length) {
        return 0;
    }
    p = realloc(c->data, c->length);
    if (!p) {
        return -1;
    }
    c->data = (uint8_t *)p;
    c->capacity = c->length;
    return 0;
}

/** 从 data 起解析；返回下一次调用的续跑位置（跨调用保留状态）。 */
static const uint8_t *rtsp_ic_scan(rtsp_ic_ctx *c, const uint8_t *data, const uint8_t *end)
{
    while (data < end) {
        switch (c->state) {
        case RTSP_IC_START:
            if (*data != '$') {
                return end; /* 非 interleaved 数据，交给调用方丢弃 */
            }
            c->bytes = 0;
            c->state = RTSP_IC_CHANNEL;
            data++;
            break;

        case RTSP_IC_CHANNEL:
            c->channel = *data++;
            c->state = RTSP_IC_LEN_HI;
            break;

        case RTSP_IC_LEN_HI:
            c->length = (uint16_t)((uint16_t)*data++ << 8);
            c->state = RTSP_IC_LEN_LO;
            break;

        case RTSP_IC_LEN_LO:
            c->length |= *data++;
            c->state = RTSP_IC_DATA;
            break;

        case RTSP_IC_DATA: {
            size_t n = (size_t)(end - data);
            size_t want;

            if (c->bytes == 0 && rtsp_ic_alloc(c) != 0) {
                return end;
            }
            want = (size_t)c->length - c->bytes;
            if (n > want) {
                n = want;
            }
            memcpy(c->data + c->bytes, data, n);
            c->bytes = (uint16_t)(c->bytes + n);
            data += n;

            if (c->bytes == c->length) {
                c->state = RTSP_IC_START;
                rtsp_ic_onrtp(c, c->channel, c->data, c->length);
                return data;
            }
            break;
        }

        default:
            return end;
        }
    }
    return data;
}

static void *rtsp_ic_create(const zcs_container_demux_opts *opts)
{
    rtsp_ic_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (rtsp_ic_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    c->state = RTSP_IC_START;
    return c;
}

static void rtsp_ic_destroy(void *ctx)
{
    rtsp_ic_ctx *c = (rtsp_ic_ctx *)ctx;

    if (!c) {
        return;
    }
    free(c->data);
    free(c);
}

static zn_err_t rtsp_ic_feed(void *ctx, const uint8_t *buf, size_t len)
{
    rtsp_ic_ctx *c = (rtsp_ic_ctx *)ctx;

    if (!c || (!buf && len)) {
        return ZN_ERR_INVALID;
    }
    if (len == 0) {
        return ZN_OK;
    }

    /* 空闲时跳过前导垃圾字节；解析中则跨调用续跑。 */
    if (c->state == RTSP_IC_START && buf[0] != '$') {
        const uint8_t *mark = (const uint8_t *)memchr(buf, '$', len);
        if (!mark) {
            return ZN_OK;
        }
        len -= (size_t)(mark - buf);
        buf = mark;
    }
    (void)rtsp_ic_scan(c, buf, buf + len);
    return ZN_OK;
}

static zn_err_t rtsp_ic_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                                   uint32_t tag_dts_ms)
{
    (void)ctx;
    (void)type_id;
    (void)body;
    (void)len;
    (void)tag_dts_ms;
    return ZN_ERR_NOT_IMPL;
}

const zcs_container_demuxer_ops zcs_container_rtsp_interleaved_ops = {
    ZCS_CONTAINER_RTSP_INTERLEAVED,
    "rtsp-interleaved",
    rtsp_ic_create,
    rtsp_ic_destroy,
    rtsp_ic_feed,
    rtsp_ic_input_tag,
};
