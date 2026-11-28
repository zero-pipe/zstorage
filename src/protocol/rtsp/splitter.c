/**
 * @file splitter.c
 * @brief RTSP 输入分流：区分 `$` interleaved 帧与 RTSP 文本消息。
 */
#include "zcs/protocol/rtsp/splitter.h"
#include "zcs/media/container/container_registry.h"
#include "zcs/media/wire/rtp_packet.h"
#include "zcs/protocol/http/message_framer.h"
#include <stdlib.h>
#include <string.h>

#define ZCS_RTSP_SPLITTER_MAX_CACHE (256U * 1024U)
/** RTP 模式下残留缓存超过该上限仍找不到帧界，则主动重新对齐到下一个 '$'。 */
#define ZCS_RTSP_RESYNC_BYTES (64U * 1024U)

struct zcs_rtsp_splitter {
    zcs_rtsp_splitter_opts opts;
    zcs_http_message_framer *inner;
    zcs_rtsp_message msg;
    int enable_rtp;
    int is_rtp_packet;
};

static const char *search_rtsp_tail_l(zcs_rtsp_splitter *s, const char *data, size_t len)
{
    if (s->enable_rtp && len > 0 && (uint8_t)data[0] == '$') {
        if (len < ZCS_RTSP_INTERLEAVED_HDR) {
            return NULL;
        }
        uint16_t plen = (uint16_t)(((uint8_t)data[2] << 8) | (uint8_t)data[3]);
        size_t total = ZCS_RTSP_INTERLEAVED_HDR + plen;
        if (len < total) {
            return NULL;
        }
        s->is_rtp_packet = 1;
        return data + total;
    }

    s->is_rtp_packet = 0;
    {
        const char *t = zcs_http_message_search_tail(data, len);

        /*
         * 安全网：RTP 模式下正常不会出现超过 64KB 的“RTSP 文本”积压。
         * 一旦积到这个量级仍没找到头部结束符，说明已经错位：
         * 直接跳到下个 '$' 重新对齐，避免一直等到缓存溢出（最多 256KB）才恢复。
         */
        if (!t && s->enable_rtp && len > ZCS_RTSP_RESYNC_BYTES) {
            const char *mark = (const char *)memchr(data, '$', len);

            if (mark && mark > data) {
                return mark;
            }
        }
        return t;
    }
}

static const char *on_search_tail(const char *data, size_t len, void *user)
{
    zcs_rtsp_splitter *s = (zcs_rtsp_splitter *)user;
    const char *ret = search_rtsp_tail_l(s, data, len);
    if (ret) {
        return ret;
    }

    if (len > ZCS_RTSP_SPLITTER_MAX_CACHE) {
        const char *mark = (const char *)memchr(data, '$', len);
        if (!mark) {
            return NULL;
        }
        return mark;
    }
    return NULL;
}

static void dispatch_interleaved(zcs_rtsp_splitter *s, const uint8_t *data, size_t len)
{
    zcs_container_packet pkt;
    if (zcs_container_rtsp_interleaved_parse(data, len, &pkt) != ZN_OK) {
        return;
    }
    if (s->opts.on_rtp) {
        s->opts.on_rtp(pkt.channel, pkt.data, pkt.len, s->opts.user);
    }
}

static void dispatch_rtsp_message(zcs_rtsp_splitter *s)
{
    if (s->opts.on_message) {
        s->opts.on_message(&s->msg, s->opts.user);
    }
    zcs_rtsp_message_clear(&s->msg);
}

static intptr_t on_rtsp_header(const char *header, size_t header_len, void *user)
{
    zcs_rtsp_splitter *s = (zcs_rtsp_splitter *)user;
    if (!s) {
        return 0;
    }

    if (s->is_rtp_packet) {
        dispatch_interleaved(s, (const uint8_t *)header, header_len);
        return 0;
    }

    if (header_len == 4 && memcmp(header, "\r\n\r\n", 4) == 0) {
        return 0;
    }

    zcs_rtsp_message_clear(&s->msg);
    if (zcs_rtsp_message_parse_header(header, header_len, &s->msg) != ZN_OK) {
        if (s->enable_rtp) {
            return 0;
        }
        return 0;
    }

    int clen = zcs_rtsp_message_content_length(&s->msg);
    if (clen <= 0) {
        dispatch_rtsp_message(s);
        return 0;
    }
    return (intptr_t)clen;
}

static void on_rtsp_content(const char *data, size_t len, void *user)
{
    zcs_rtsp_splitter *s = (zcs_rtsp_splitter *)user;
    if (!s) {
        return;
    }
    s->msg.msg.body = data;
    s->msg.msg.body_len = len;
    dispatch_rtsp_message(s);
}

zcs_rtsp_splitter *zcs_rtsp_splitter_create(const zcs_rtsp_splitter_opts *opts)
{
    if (!opts) {
        return NULL;
    }
    zcs_rtsp_splitter *s = (zcs_rtsp_splitter *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->opts = *opts;

    zcs_http_message_framer_opts iopts = {
        .on_header = on_rtsp_header,
        .on_content = on_rtsp_content,
        .user = s,
        .max_cache_size = ZCS_RTSP_SPLITTER_MAX_CACHE,
        .on_search_tail = on_search_tail,
    };
    s->inner = zcs_http_message_framer_create(&iopts);
    if (!s->inner) {
        free(s);
        return NULL;
    }
    return s;
}

void zcs_rtsp_splitter_destroy(zcs_rtsp_splitter *s)
{
    if (!s) {
        return;
    }
    zcs_http_message_framer_destroy(s->inner);
    zcs_rtsp_message_clear(&s->msg);
    free(s);
}

void zcs_rtsp_splitter_enable_rtp(zcs_rtsp_splitter *s, int on)
{
    if (s) {
        s->enable_rtp = on ? 1 : 0;
    }
}

void zcs_rtsp_splitter_reset(zcs_rtsp_splitter *s)
{
    if (!s) {
        return;
    }
    zcs_http_message_framer_reset(s->inner);
    zcs_rtsp_message_clear(&s->msg);
    s->is_rtp_packet = 0;
}

zn_err_t zcs_rtsp_splitter_input(zcs_rtsp_splitter *s, const void *data, size_t len)
{
    if (!s || !s->inner) {
        return ZN_ERR_INVALID;
    }
    if (!data && len) {
        return ZN_ERR_INVALID;
    }
    if (len == 0) {
        return ZN_OK;
    }

    zn_err_t err = zcs_http_message_framer_input(s->inner, data, len);
    if (err == ZN_ERR_BUFFER_TOO_SMALL && s->enable_rtp) {
        zcs_rtsp_splitter_reset(s);
        const char *d = (const char *)data;
        const char *mark = (const char *)memchr(d, '$', len);
        if (mark && mark > d) {
            return zcs_http_message_framer_input(s->inner, mark, len - (size_t)(mark - d));
        }
        return ZN_ERR_IO;
    }
    return err;
}
