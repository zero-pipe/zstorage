/**
 * @file message_framer.c
 * @brief HTTP 消息边界组帧（头 + Content-Length/chunked）。
 */
#include "zcs/protocol/http/message_framer.h"
#include "zn/util/byte_buf.h"
#include <stdlib.h>
#include <string.h>

#define ZCS_HTTP_DEFAULT_MAX_CACHE (4U * 1024U * 1024U)

struct zcs_http_message_framer {
    zcs_http_message_framer_opts opts;
    zn_byte_buf *remain;
    intptr_t content_len;
    size_t remain_data_size;
    size_t max_cache_size;
};

const char *zcs_http_message_search_tail(const char *data, size_t len)
{
    for (size_t i = 0; i + 3 < len; ++i) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            return data + i + 4;
        }
    }
    return NULL;
}

static const char *search_packet_tail(zcs_http_message_framer *s, const char *data, size_t len)
{
    if (s->opts.on_search_tail) {
        return s->opts.on_search_tail(data, len, s->opts.user);
    }
    return zcs_http_message_search_tail(data, len);
}

zcs_http_message_framer *zcs_http_message_framer_create(const zcs_http_message_framer_opts *opts)
{
    zcs_http_message_framer *s = (zcs_http_message_framer *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    if (opts) {
        s->opts = *opts;
    }
    s->max_cache_size =
        s->opts.max_cache_size ? s->opts.max_cache_size : ZCS_HTTP_DEFAULT_MAX_CACHE;
    s->remain = zn_byte_buf_create(0);
    if (!s->remain) {
        free(s);
        return NULL;
    }
    return s;
}

void zcs_http_message_framer_destroy(zcs_http_message_framer *s)
{
    if (!s) {
        return;
    }
    zn_byte_buf_destroy(s->remain);
    free(s);
}

void zcs_http_message_framer_reset(zcs_http_message_framer *s)
{
    if (!s) {
        return;
    }
    s->content_len = 0;
    s->remain_data_size = 0;
    zn_byte_buf_clear(s->remain);
}

size_t zcs_http_message_framer_remain_size(const zcs_http_message_framer *s)
{
    return s ? s->remain_data_size : 0;
}

/**
 * 保留 [ptr, ptr+n) 作为下一轮的残留字节。
 *
 * ptr 可能指向调用方的输入 chunk，也可能就在 remain 自身里（回拼路径）。
 * 后者不能用 clear+append：因为 zn_byte_buf_clear() 会把 data[0] 写成 '\0'，
 * 而紧接着要从同一块内存把剩余字节拷回开头，首字节就被抹成 0。
 * RTSP interleaved 的 '$'(0x24) 一旦被抹成 0，分帧就永远找不到帧头，
 * 直到缓存满溢才重新对齐——表现为录像中间断流几秒到十几秒。
 */
static zn_err_t framer_keep_tail(zcs_http_message_framer *s, const char *ptr, size_t n,
                                 int in_remain)
{
    if (in_remain) {
        size_t off = (size_t)(ptr - zn_byte_buf_data(s->remain));

        return zn_byte_buf_erase_front(s->remain, off);
    }
    zn_byte_buf_clear(s->remain);
    return zn_byte_buf_append(s->remain, ptr, n);
}

zn_err_t zcs_http_message_framer_input(zcs_http_message_framer *s, const void *data, size_t len)
{
    if (!s || !data || len == 0) {
        return ZN_ERR_INVALID;
    }

    size_t cached = zn_byte_buf_size(s->remain);
    if (cached + len > s->max_cache_size) {
        zcs_http_message_framer_reset(s);
        return ZN_ERR_BUFFER_TOO_SMALL;
    }

    const char *ptr = (const char *)data;
    int in_remain = 0;
    if (cached > 0) {
        if (zn_byte_buf_append(s->remain, data, len) != ZN_OK) {
            return ZN_ERR_NOMEM;
        }
        ptr = zn_byte_buf_data(s->remain);
        len = zn_byte_buf_size(s->remain);
        in_remain = 1;
    }

split_packet:
    ; /* C99: a label must be followed by a statement, not a declaration */
    const char *packet_data = ptr;
    s->remain_data_size = len;

    while (s->content_len == 0 && s->remain_data_size > 0) {
        const char *index = search_packet_tail(s, ptr, s->remain_data_size);
        if (!index || index == ptr) {
            break;
        }
        if (index < ptr || index > ptr + s->remain_data_size) {
            return ZN_ERR_INVALID;
        }

        const char *header_ptr = ptr;
        size_t header_size = (size_t)(index - ptr);
        ptr = index;
        s->remain_data_size = len - (size_t)(ptr - packet_data);

        if (s->opts.on_header) {
            s->content_len = s->opts.on_header(header_ptr, header_size, s->opts.user);
        } else {
            s->content_len = 0;
        }
    }

    if (s->remain_data_size <= 0) {
        zn_byte_buf_clear(s->remain);
        return ZN_OK;
    }

    if (s->content_len == 0) {
        if (framer_keep_tail(s, ptr, s->remain_data_size, in_remain) != ZN_OK) {
            return ZN_ERR_NOMEM;
        }
        return ZN_OK;
    }

    if (s->content_len > 0) {
        if (s->remain_data_size < (size_t)s->content_len) {
            if (framer_keep_tail(s, ptr, s->remain_data_size, in_remain) != ZN_OK) {
                return ZN_ERR_NOMEM;
            }
            return ZN_OK;
        }
        if (s->opts.on_content) {
            s->opts.on_content(ptr, (size_t)s->content_len, s->opts.user);
        }
        s->remain_data_size -= (size_t)s->content_len;
        ptr += (size_t)s->content_len;
        s->content_len = 0;

        if (s->remain_data_size > 0) {
            if (framer_keep_tail(s, ptr, s->remain_data_size, in_remain) != ZN_OK) {
                return ZN_ERR_NOMEM;
            }
            ptr = zn_byte_buf_data(s->remain);
            len = zn_byte_buf_size(s->remain);
            in_remain = 1;
            goto split_packet;
        }
        zn_byte_buf_clear(s->remain);
        return ZN_OK;
    }

    if (s->opts.on_content) {
        s->opts.on_content(ptr, s->remain_data_size, s->opts.user);
    }
    zn_byte_buf_clear(s->remain);
    s->remain_data_size = 0;
    return ZN_OK;
}
