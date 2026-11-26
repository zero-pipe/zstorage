/**
 * @file sink.c
 * @brief 播放输出端统一取数驱动（见 sink.h 的契约说明）。
 */
#include "zms/play/sink.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include <string.h>

int zms_sink_pull(zms_sink_next_fn next, void *muxer, struct zn_poller *pol, size_t init_cap,
                  size_t max_cap, zn_buf **out, size_t *out_len)
{
    zn_buf *buf;
    size_t cap;
    size_t n = 0;
    int r;

    if (!next || !out || !out_len) {
        return ZN_ERR_INVALID;
    }
    *out = NULL;
    *out_len = 0;
    cap = init_cap ? init_cap : (64u * 1024u);

    buf = zn_buf_alloc_local(pol, cap);
    if (!buf) {
        return ZN_ERR_NOMEM;
    }
    r = next(muxer, (uint8_t *)zn_buf_data(buf), zn_buf_cap(buf), &n);
    if (r == ZMS_SINK_NEED_BIGGER) {
        size_t need = n;

        zn_buf_unref(buf);
        if (need == 0 || (max_cap && need > max_cap)) {
            zn_warn("sink: chunk needs %u bytes, over limit %u",
                    (unsigned)need, (unsigned)max_cap);
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        buf = zn_buf_alloc_local(pol, need);
        if (!buf) {
            return ZN_ERR_NOMEM;
        }
        n = 0;
        r = next(muxer, (uint8_t *)zn_buf_data(buf), zn_buf_cap(buf), &n);
        if (r == ZMS_SINK_NEED_BIGGER) {
            /* 契约要求：给足所需字节后必须能取出同一块数据。 */
            zn_warn("sink: still needs more after resize (%u bytes)", (unsigned)n);
            zn_buf_unref(buf);
            return ZN_ERR_STATE;
        }
    }
    if (r <= 0 || n == 0) {
        zn_buf_unref(buf);
        return r < 0 ? r : 0;
    }
    zn_buf_set_len(buf, n);
    *out = buf;
    *out_len = n;
    return r;
}
