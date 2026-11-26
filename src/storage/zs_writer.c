/**
 * @file zs_writer.c
 * @brief 单通道写入器实现。
 */
#include "storage/zs_writer.h"

#include <stdlib.h>
#include <string.h>

#define ZS_WRITER_FLUSH_BYTES_DEF (512u * 1024u)
#define ZS_WRITER_FLUSH_MS_DEF 1000u
#define ZS_WRITER_SLOT_MAX_MS_DEF (15u * 60u * 1000u)
#define ZS_WRITER_SEAL_MARGIN_DEF (4u * 1024u * 1024u)

struct zs_writer {
    zs_pool *pool;
    uint32_t channel_id;
    char stream[ZS_SLOT_STREAM_MAX];
    uint32_t flush_bytes;
    uint32_t flush_ms;
    uint32_t slot_max_ms;
    uint64_t seal_margin;

    uint8_t *buf;
    uint32_t buf_used;

    zs_pool_lease lease;
    uint64_t slot_start_ms;
    uint64_t last_flush_ms;

    int pending_key;
    int64_t pending_pts;
    int64_t pending_utc;

    int stalled; /* 当前槽位写满且不在关键帧边界：丢弃直到下一个关键帧 */
    uint32_t rotations;
    uint64_t total_bytes;
};

zs_writer *zs_writer_create(const zs_writer_opts *opts)
{
    zs_writer *w;
    uint32_t flush_bytes;

    if (!opts || !opts->pool || !opts->stream || !opts->stream[0]) {
        return NULL;
    }
    flush_bytes = opts->flush_bytes ? opts->flush_bytes : ZS_WRITER_FLUSH_BYTES_DEF;
    w = (zs_writer *)calloc(1, sizeof(*w));
    if (!w) {
        return NULL;
    }
    w->buf = (uint8_t *)malloc(flush_bytes);
    if (!w->buf) {
        free(w);
        return NULL;
    }
    w->pool = opts->pool;
    w->channel_id = opts->channel_id;
    strncpy(w->stream, opts->stream, sizeof(w->stream) - 1u);
    w->flush_bytes = flush_bytes;
    w->flush_ms = opts->flush_ms ? opts->flush_ms : ZS_WRITER_FLUSH_MS_DEF;
    w->slot_max_ms = opts->slot_max_ms ? opts->slot_max_ms : ZS_WRITER_SLOT_MAX_MS_DEF;
    w->seal_margin = opts->slot_seal_margin ? opts->slot_seal_margin : ZS_WRITER_SEAL_MARGIN_DEF;
    return w;
}

void zs_writer_destroy(zs_writer *w)
{
    if (!w) {
        return;
    }
    zs_writer_close(w);
    free(w->buf);
    free(w);
}

/* 把缓冲刷进当前槽位并同步池元数据。 */
static int zs_writer_flush_buf(zs_writer *w, uint64_t now_ms)
{
    if (!w->lease.slot) {
        w->buf_used = 0;
        return 0;
    }
    if (w->buf_used > 0) {
        if (zs_slot_append(w->lease.slot, w->buf, w->buf_used) != 0) {
            w->buf_used = 0;
            return -1;
        }
        w->buf_used = 0;
    }
    (void)zs_pool_sync(w->pool, &w->lease);
    w->last_flush_ms = now_ms;
    return 0;
}

/* 封口当前槽位并申请新槽位。 */
static int zs_writer_rotate(zs_writer *w, uint64_t now_ms)
{
    if (w->lease.slot) {
        (void)zs_writer_flush_buf(w, now_ms);
        (void)zs_pool_release(w->pool, &w->lease);
        w->slot_start_ms = 0;
    }
    w->stalled = 0;
    if (zs_pool_acquire(w->pool, w->channel_id, w->stream, now_ms, &w->lease) != 0) {
        return -1;
    }
    w->slot_start_ms = now_ms;
    w->last_flush_ms = now_ms;
    w->rotations++;
    return 0;
}

static int zs_writer_need_rotate(const zs_writer *w, uint64_t now_ms)
{
    uint64_t remaining;

    if (!w->lease.slot) {
        return 1;
    }
    if (w->slot_max_ms > 0 && now_ms > w->slot_start_ms + w->slot_max_ms) {
        return 1;
    }
    remaining = zs_slot_remaining(w->lease.slot);
    if (remaining <= w->seal_margin + (uint64_t)w->buf_used) {
        return 1;
    }
    return 0;
}

void zs_writer_begin_frame(zs_writer *w, int keyframe, int64_t pts_90k, int64_t utc_ms,
                           uint64_t now_ms)
{
    if (!w) {
        return;
    }
    if (!w->lease.slot) {
        /* 还没开槽：只有关键帧能开，否则整帧丢弃 */
        if (!keyframe) {
            w->pending_key = 0;
            return;
        }
        (void)zs_writer_rotate(w, now_ms);
    } else if (keyframe && zs_writer_need_rotate(w, now_ms)) {
        (void)zs_writer_rotate(w, now_ms);
    }
    if (!w->lease.slot) {
        w->pending_key = 0;
        return;
    }
    w->pending_key = keyframe ? 1 : 0;
    w->pending_pts = pts_90k;
    w->pending_utc = utc_ms;
}

int zs_writer_push(zs_writer *w, const void *ps, size_t len, uint64_t now_ms)
{
    uint64_t off;

    if (!w || (!ps && len)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!w->lease.slot || w->stalled) {
        w->pending_key = 0;
        return 0; /* 等待关键帧或等待下一个槽位，本包丢弃 */
    }

    /* 大包（超过整个缓冲）直接写，避免额外一次拷贝 */
    if (len > w->flush_bytes) {
        if (zs_writer_flush_buf(w, now_ms) != 0) {
            return -1;
        }
        if (w->pending_key) {
            off = zs_slot_write_offset(w->lease.slot) + w->buf_used;
            (void)zs_slot_add_keyframe(w->lease.slot, (uint64_t)w->pending_utc, w->pending_pts, off,
                                       0);
            w->pending_key = 0;
        }
        if (zs_slot_append(w->lease.slot, ps, len) != 0) {
            w->stalled = 1;
            return -1;
        }
        w->total_bytes += len;
        return 0;
    }

    if ((size_t)w->buf_used + len > (size_t)w->flush_bytes) {
        if (zs_writer_flush_buf(w, now_ms) != 0) {
            return -1;
        }
    }
    if (w->pending_key) {
        off = zs_slot_write_offset(w->lease.slot) + w->buf_used;
        (void)zs_slot_add_keyframe(w->lease.slot, (uint64_t)w->pending_utc, w->pending_pts, off, 0);
        w->pending_key = 0;
    }
    memcpy(w->buf + w->buf_used, ps, len);
    w->buf_used += (uint32_t)len;
    w->total_bytes += len;
    if (w->buf_used >= w->flush_bytes) {
        if (zs_writer_flush_buf(w, now_ms) != 0) {
            return -1;
        }
    }
    return 0;
}

int zs_writer_tick(zs_writer *w, uint64_t now_ms)
{
    if (!w || !w->lease.slot) {
        return 0;
    }
    if (w->buf_used == 0) {
        return 0;
    }
    if (now_ms < w->last_flush_ms + w->flush_ms) {
        return 0;
    }
    return zs_writer_flush_buf(w, now_ms);
}

int zs_writer_flush(zs_writer *w, uint64_t now_ms)
{
    if (!w) {
        return -1;
    }
    return zs_writer_flush_buf(w, now_ms);
}

void zs_writer_close(zs_writer *w)
{
    if (!w) {
        return;
    }
    if (w->lease.slot) {
        (void)zs_writer_flush_buf(w, w->last_flush_ms);
        (void)zs_pool_release(w->pool, &w->lease);
        w->slot_start_ms = 0;
    }
    w->buf_used = 0;
    w->pending_key = 0;
}

uint32_t zs_writer_slot_id(const zs_writer *w)
{
    return (w && w->lease.slot) ? w->lease.id : 0u;
}

uint32_t zs_writer_rotations(const zs_writer *w)
{
    return w ? w->rotations : 0u;
}

uint64_t zs_writer_bytes(const zs_writer *w)
{
    return w ? w->total_bytes : 0;
}

int zs_writer_active(const zs_writer *w)
{
    return (w && w->lease.slot) ? 1 : 0;
}
