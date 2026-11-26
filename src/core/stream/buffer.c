/**
 * @file buffer.c
 * @brief 点播播放缓冲与每会话读取者。
 *
 * 实现是一个 seq 递增的环形缓冲：写入端（解复用线程）写 write_seq 指向的槽并递增；
 * 每个读取者有独立 read_seq，按自己的进度消费。consumed_seq 记录“所有读者中已推进
 * 到的最远位置”，用于计算可回收的最旧边界（oldest = write_seq - cap）以及写入端的
 * “写满”判断：当 write_seq 与 consumed_seq 差距达到 cap，说明有读者严重落后，
 * 写入端返回 BUFFER_TOO_SMALL 作为背压信号，而不是覆盖尚未读的槽。
 *
 * 锁：所有槽与 seq 的读写都在 fifo->mtx 下进行。读取者各持 read_seq，只有推进时
 * 才进锁更新 consumed_seq。读取可在锁外访问返回的 data 指针，前提是槽在被覆盖前
 * 已被 consumed_seq 推进过（写入端据此延后清理）。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/core/stream/buffer.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/core/frame.h"
#include "zms/util/buf_pool.h"
#include "zn/poller/poller.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t len;
    zms_codec_id codec;
    zms_track_type track;
    uint32_t dts_ms;
    uint32_t pts_ms;
    int keyframe;
    int config_frame;
} vod_slot;

struct zms_vod_buffer {
    vod_slot *slots;
    size_t cap;
    uint64_t write_seq;
    uint64_t consumed_seq;
    /* config 与直播 gop_queue 一致，使用定长双缓冲：返回给读侧的指针始终落在
     * 缓冲体内，只会随 point-play 源一起释放；不会因重开/更新配置变成悬空指针
     * （点播解复用线程写、播放/egress 线程读是跨线程的）。 */
    uint8_t video_config[2][ZMS_VOD_BUFFER_CONFIG_MAX];
    size_t video_config_len;
    unsigned video_config_active;
    uint8_t audio_config[2][ZMS_VOD_BUFFER_CONFIG_MAX];
    size_t audio_config_len;
    unsigned audio_config_active;
    zb_mutex *mtx;
};

struct zms_vod_buffer_reader {
    zms_vod_buffer *fifo;
    uint64_t read_seq;
};

/* 写 config 到非活动缓冲再切换（调用方持 mtx），与 gop_queue 的 set_blob 语义一致。 */
static int set_blob(uint8_t (*bufs)[ZMS_VOD_BUFFER_CONFIG_MAX], unsigned *active, size_t *len,
                    const void *data, size_t data_len)
{
    unsigned next;

    if (!bufs || !active || !len) {
        return -1;
    }
    if (data_len == 0) {
        *len = 0;
        return 0;
    }
    if (data_len > ZMS_VOD_BUFFER_CONFIG_MAX) {
        return -1;
    }
    next = *active ^ 1u;
    memcpy(bufs[next], data, data_len);
    *len = data_len;
    *active = next;
    return 0;
}

static void slot_clear(vod_slot *s)
{
    if (!s) {
        return;
    }
    zms_buf_pool_slot_clear(&s->data, &s->cap);
    s->len = 0;
    s->codec = ZMS_CODEC_INVALID;
    s->track = ZMS_TRACK_INVALID;
    s->dts_ms = 0;
    s->pts_ms = 0;
    s->keyframe = 0;
    s->config_frame = 0;
}

zms_vod_buffer *zms_vod_buffer_create(size_t cap)
{
    zms_vod_buffer *f;

    if (cap == 0) {
        cap = ZMS_VOD_BUFFER_DEFAULT_CAP;
    }
    f = (zms_vod_buffer *)calloc(1, sizeof(*f));
    if (!f) {
        return NULL;
    }
    f->slots = (vod_slot *)calloc(cap, sizeof(vod_slot));
    if (!f->slots) {
        free(f);
        return NULL;
    }
    f->cap = cap;
    f->mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!f->mtx) {
        free(f->slots);
        free(f);
        return NULL;
    }
    return f;
}

void zms_vod_buffer_destroy(zms_vod_buffer *fifo)
{
    size_t i;

    if (!fifo) {
        return;
    }
    if (fifo->mtx) {
        zb_mutex_lock(fifo->mtx);
    }
    for (i = 0; i < fifo->cap; ++i) {
        slot_clear(&fifo->slots[i]);
    }
    free(fifo->slots);
    if (fifo->mtx) {
        zb_mutex_unlock(fifo->mtx);
        zb_mutex_destroy(fifo->mtx);
    }
    free(fifo);
}

static uint64_t min_read_seq_locked(const zms_vod_buffer *fifo)
{
    return fifo ? fifo->consumed_seq : 0;
}

/*
 * 写入一帧。两个关键决策：
 *   1) 满则背压：write_seq 领先 consumed_seq 达 cap 时返回 BUFFER_TOO_SMALL，让解复用
 *      放慢，绝不覆盖读者尚未推进到的槽（否则读者会读到已被覆写的错帧）。
 *   2) owned 帧直接接管（不拷贝）：调用方已把缓冲所有权转移过来，省一次大块 memcpy；
 *      否则 resize+拷贝。失败时回滚 write_seq，保证 seq 与已写入数量一致。
 * 末尾的 oldest 清理只在“该槽确实已被挤出窗口”时清空，避免清掉仍可能被读的槽。
 */
zn_err_t zms_vod_buffer_write(zms_vod_buffer *fifo, const zms_frame *frame)
{
    vod_slot *s;
    uint64_t seq;
    size_t oldest;

    if (!fifo || !frame || !frame->data || frame->size == 0) {
        return ZN_ERR_INVALID;
    }

    zb_mutex_lock(fifo->mtx);
    if (fifo->write_seq > fifo->consumed_seq &&
        (fifo->write_seq - fifo->consumed_seq) >= fifo->cap) {
        zb_mutex_unlock(fifo->mtx);
        return ZN_ERR_BUFFER_TOO_SMALL;
    }

    seq = fifo->write_seq++;
    s = &fifo->slots[seq % fifo->cap];
    slot_clear(s);
    if (frame->owned && frame->data && frame->capacity >= frame->size) {
        s->data = frame->data;
        s->cap = frame->capacity;
        s->len = frame->size;
    } else {
        if (!zms_buf_pool_slot_resize(&s->data, &s->cap, frame->size)) {
            fifo->write_seq--;
            zb_mutex_unlock(fifo->mtx);
            return ZN_ERR_NOMEM;
        }
        memcpy(s->data, frame->data, frame->size);
        s->len = frame->size;
    }
    s->codec = frame->codec;
    s->track = frame->track;
    zms_gop_queue_timeline_from_frame(frame, &s->dts_ms, &s->pts_ms);
    s->keyframe = frame->keyframe;
    s->config_frame = frame->config_frame;

    oldest = (fifo->write_seq > fifo->cap) ? (fifo->write_seq - fifo->cap) : 0;
    if (oldest > 0) {
        vod_slot *drop = &fifo->slots[oldest % fifo->cap];
        if (drop->data && oldest + fifo->cap <= fifo->write_seq) {
            slot_clear(drop);
        }
    }
    zb_mutex_unlock(fifo->mtx);
    return ZN_OK;
}

zn_err_t zms_vod_buffer_set_video_config(zms_vod_buffer *fifo, const void *data, size_t len)
{
    if (!fifo) {
        return ZN_ERR_INVALID;
    }
    zb_mutex_lock(fifo->mtx);
    if (set_blob(fifo->video_config, &fifo->video_config_active, &fifo->video_config_len, data,
                 len) != 0) {
        zb_mutex_unlock(fifo->mtx);
        return ZN_ERR_NOMEM;
    }
    zb_mutex_unlock(fifo->mtx);
    return ZN_OK;
}

zn_err_t zms_vod_buffer_set_audio_config(zms_vod_buffer *fifo, const void *data, size_t len)
{
    if (!fifo) {
        return ZN_ERR_INVALID;
    }
    zb_mutex_lock(fifo->mtx);
    if (set_blob(fifo->audio_config, &fifo->audio_config_active, &fifo->audio_config_len, data,
                 len) != 0) {
        zb_mutex_unlock(fifo->mtx);
        return ZN_ERR_NOMEM;
    }
    zb_mutex_unlock(fifo->mtx);
    return ZN_OK;
}

void zms_vod_buffer_reset(zms_vod_buffer *fifo)
{
    size_t i;

    if (!fifo) {
        return;
    }
    zb_mutex_lock(fifo->mtx);
    for (i = 0; i < fifo->cap; ++i) {
        slot_clear(&fifo->slots[i]);
    }
    fifo->write_seq = 0;
    fifo->consumed_seq = 0;
    zb_mutex_unlock(fifo->mtx);
}

void zms_vod_buffer_reader_seek_beginning(zms_vod_buffer_reader *rd)
{
    if (!rd || !rd->fifo) {
        return;
    }
    zb_mutex_lock(rd->fifo->mtx);
    rd->read_seq =
        (rd->fifo->write_seq > rd->fifo->cap) ? (rd->fifo->write_seq - rd->fifo->cap) : 0;
    zb_mutex_unlock(rd->fifo->mtx);
}

const uint8_t *zms_vod_buffer_video_config(const zms_vod_buffer *fifo, size_t *len)
{
    if (!fifo) {
        return NULL;
    }
    if (len) {
        *len = fifo->video_config_len;
    }
    return fifo->video_config_len > 0 ? fifo->video_config[fifo->video_config_active] : NULL;
}

const uint8_t *zms_vod_buffer_audio_config(const zms_vod_buffer *fifo, size_t *len)
{
    if (!fifo) {
        return NULL;
    }
    if (len) {
        *len = fifo->audio_config_len;
    }
    return fifo->audio_config_len > 0 ? fifo->audio_config[fifo->audio_config_active] : NULL;
}

size_t zms_vod_buffer_pending(const zms_vod_buffer *fifo)
{
    size_t n;

    if (!fifo) {
        return 0;
    }
    zb_mutex_lock(fifo->mtx);
    n = (fifo->write_seq > fifo->consumed_seq) ? (size_t)(fifo->write_seq - fifo->consumed_seq) : 0;
    zb_mutex_unlock(fifo->mtx);
    return n;
}

int zms_vod_buffer_has_h264_idr(const zms_vod_buffer *fifo)
{
    return zms_vod_buffer_has_video_sync_key(fifo);
}

int zms_vod_buffer_has_video_sync_key(const zms_vod_buffer *fifo)
{
    uint64_t seq;
    uint64_t oldest;

    if (!fifo) {
        return 0;
    }
    zb_mutex_lock(fifo->mtx);
    if (fifo->write_seq == 0) {
        zb_mutex_unlock(fifo->mtx);
        return 0;
    }
    oldest = (fifo->write_seq > fifo->cap) ? (fifo->write_seq - fifo->cap) : 0;
    for (seq = oldest; seq < fifo->write_seq; ++seq) {
        const vod_slot *s = &fifo->slots[seq % fifo->cap];
        if (s->track != ZMS_TRACK_VIDEO || !s->data || s->len == 0) {
            continue;
        }
        if (s->codec == ZMS_CODEC_H264 && (zms_h264_annexb_is_idr(s->data, s->len) ||
                                           zms_h264_annexb_is_sync_key(s->data, s->len))) {
            zb_mutex_unlock(fifo->mtx);
            return 1;
        }
        if (s->codec == ZMS_CODEC_H265 && (zms_h265_annexb_is_idr(s->data, s->len) ||
                                           zms_h265_annexb_is_sync_key(s->data, s->len))) {
            zb_mutex_unlock(fifo->mtx);
            return 1;
        }
    }
    zb_mutex_unlock(fifo->mtx);
    return 0;
}

/* 附加一个读取者。from_beginning=1 从当前窗口最旧处开始（回看/补关键帧），
 * 否则从 write_seq（最新）开始——播放起播通常要的是从某个关键帧开始的连续数据，
 * 由上层结合 seek_video_key 决定。 */
zms_vod_buffer_reader *zms_vod_buffer_reader_attach(zms_vod_buffer *fifo, int from_beginning)
{
    zms_vod_buffer_reader *rd;

    if (!fifo) {
        return NULL;
    }
    rd = (zms_vod_buffer_reader *)calloc(1, sizeof(*rd));
    if (!rd) {
        return NULL;
    }
    rd->fifo = fifo;
    zb_mutex_lock(fifo->mtx);
    if (from_beginning) {
        rd->read_seq = (fifo->write_seq > fifo->cap) ? (fifo->write_seq - fifo->cap) : 0;
    } else {
        rd->read_seq = fifo->write_seq;
    }
    zb_mutex_unlock(fifo->mtx);
    return rd;
}

void zms_vod_buffer_reader_detach(zms_vod_buffer_reader *rd)
{
    free(rd);
}

static void vod_slot_to_ring_slot(const vod_slot *s, zms_gop_slot *slot)
{
    if (!slot) {
        return;
    }
    memset(slot, 0, sizeof(*slot));
    if (!s) {
        return;
    }
    slot->codec = s->codec;
    slot->track = s->track;
    slot->dts_ms = s->dts_ms;
    slot->pts_ms = s->pts_ms;
    slot->keyframe = s->keyframe;
    slot->config_frame = s->config_frame;
    slot->data = s->data;
    slot->len = s->len;
}

/*
 * 取 read_seq 处一帧。advance=0 只窥视不移位；=1 则推进并更新 consumed_seq（取
 * 各读者最远进度）。返回 0 表示无数据或空槽（调用方通常据此暂停）；1 表示拿到。
 * 注意返回的 slot->data 指向槽内缓冲，锁释放后仍有效（槽在被覆盖前已被 consumed
 * 推进保护），但下次写到该槽就会被覆盖，调用方应尽快使用。
 */
static int vod_buffer_reader_slot_at(zms_vod_buffer_reader *rd, zms_gop_slot *slot, int advance)
{
    const vod_slot *s;
    uint64_t seq;

    if (!rd || !rd->fifo || !slot) {
        return 0;
    }

    zb_mutex_lock(rd->fifo->mtx);
    if (rd->read_seq >= rd->fifo->write_seq) {
        zb_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    seq = rd->read_seq;
    if (advance) {
        rd->read_seq++;
        if (rd->read_seq > rd->fifo->consumed_seq) {
            rd->fifo->consumed_seq = rd->read_seq;
        }
    }
    s = &rd->fifo->slots[seq % rd->fifo->cap];
    if (!s->data || s->len == 0) {
        zb_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    vod_slot_to_ring_slot(s, slot);
    zb_mutex_unlock(rd->fifo->mtx);
    return 1;
}

/*
 * 窥视并“拷贝”到调用方的 es_buf（不解引用槽内缓冲）。为什么这里拷而不是像其它
 * peek 那样直接给指针：调用方（出站 egress）需要一个可长期持有、且不会被后续写入
 * 覆盖的缓冲，所以借调用方提供的池化缓冲做一份稳定副本。pol 决定从本地池分配。
 */
int zms_vod_buffer_reader_peek_muxed_es(zms_vod_buffer_reader *rd, zms_gop_slot *slot,
                                        uint8_t **es_buf, size_t *es_cap, zn_poller *pol)
{
    const vod_slot *s;
    uint64_t seq;

    if (!rd || !rd->fifo || !slot || !es_buf || !es_cap) {
        return 0;
    }

    zb_mutex_lock(rd->fifo->mtx);
    if (rd->read_seq >= rd->fifo->write_seq) {
        zb_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    seq = rd->read_seq;
    s = &rd->fifo->slots[seq % rd->fifo->cap];
    if (!s->data || s->len == 0) {
        zb_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    if (pol) {
        if (!zms_buf_pool_slot_resize_poller(es_buf, es_cap, s->len, pol)) {
            zb_mutex_unlock(rd->fifo->mtx);
            return 0;
        }
    } else if (!zms_buf_pool_slot_resize(es_buf, es_cap, s->len)) {
        zb_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    memcpy(*es_buf, s->data, s->len);
    vod_slot_to_ring_slot(s, slot);
    slot->data = *es_buf;
    slot->len = s->len;
    zb_mutex_unlock(rd->fifo->mtx);
    return 1;
}

int zms_vod_buffer_reader_read_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot)
{
    return vod_buffer_reader_slot_at(rd, slot, 1);
}

int zms_vod_buffer_reader_peek_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot)
{
    return vod_buffer_reader_slot_at(rd, slot, 0);
}

void zms_vod_buffer_reader_advance(zms_vod_buffer_reader *rd)
{
    if (!rd || !rd->fifo) {
        return;
    }
    zb_mutex_lock(rd->fifo->mtx);
    if (rd->read_seq < rd->fifo->write_seq) {
        rd->read_seq++;
        if (rd->read_seq > rd->fifo->consumed_seq) {
            rd->fifo->consumed_seq = rd->read_seq;
        }
    }
    zb_mutex_unlock(rd->fifo->mtx);
}

/* seek 到第一个视频关键帧（从当前 read_seq 往前找）。keyframe 标志优先，缺失时对
 * H264/H265 Annex-B 现探 sync；找到就把 read_seq 定位到该帧，保证后续从可解码的
 * IRAP/IDR 起播。找不到则保持原位。 */
void zms_vod_buffer_reader_seek_video_key(zms_vod_buffer_reader *rd)
{
    uint64_t seq;

    if (!rd || !rd->fifo) {
        return;
    }
    zb_mutex_lock(rd->fifo->mtx);
    for (seq = rd->read_seq; seq < rd->fifo->write_seq; ++seq) {
        const vod_slot *s = &rd->fifo->slots[seq % rd->fifo->cap];
        int sync = 0;

        if (s->track != ZMS_TRACK_VIDEO || !s->data || s->len == 0) {
            continue;
        }
        sync = s->keyframe;
        if (!sync && s->codec == ZMS_CODEC_H264) {
            sync = zms_h264_annexb_is_sync_key(s->data, s->len);
        } else if (!sync && s->codec == ZMS_CODEC_H265) {
            sync = zms_h265_annexb_is_sync_key(s->data, s->len);
        }
        if (!sync) {
            continue;
        }
        rd->read_seq = seq;
        zb_mutex_unlock(rd->fifo->mtx);
        return;
    }
    zb_mutex_unlock(rd->fifo->mtx);
}

/*
 * 按时长定位：从窗口最旧处线性扫描第一帧 dts>=ms 的位置，定位后立刻回退到最近
 * 视频关键帧（seek_video_key）——精确到 ms 的帧未必是关键帧，无法独立解码。
 * out_ms 回报实际落点（关键帧的 dts），便于上层校正 UI/进度。返回 1 命中，0 未找到。
 */
int zms_vod_buffer_reader_seek_ms(zms_vod_buffer_reader *rd, uint64_t ms, uint64_t *out_ms)
{
    uint64_t seq, oldest;
    const vod_slot *s;

    if (out_ms) {
        *out_ms = ms;
    }
    if (!rd || !rd->fifo) {
        return 0;
    }
    zb_mutex_lock(rd->fifo->mtx);
    if (rd->fifo->write_seq == 0) {
        zb_mutex_unlock(rd->fifo->mtx);
        return 0;
    }
    oldest = (rd->fifo->write_seq > rd->fifo->cap) ? (rd->fifo->write_seq - rd->fifo->cap) : 0;
    for (seq = oldest; seq < rd->fifo->write_seq; ++seq) {
        s = &rd->fifo->slots[seq % rd->fifo->cap];
        if (!s->data || s->len == 0) {
            continue;
        }
        if ((uint64_t)s->dts_ms >= ms) {
            rd->read_seq = seq;
            if (out_ms) {
                *out_ms = s->dts_ms;
            }
            zb_mutex_unlock(rd->fifo->mtx);
            zms_vod_buffer_reader_seek_video_key(rd);
            if (out_ms) {
                zms_gop_slot slot;
                if (zms_vod_buffer_reader_peek_muxed(rd, &slot)) {
                    *out_ms = slot.dts_ms;
                }
            }
            return 1;
        }
    }
    zb_mutex_unlock(rd->fifo->mtx);
    return 0;
}

size_t zms_vod_buffer_reader_lag(const zms_vod_buffer_reader *rd)
{
    if (!rd || !rd->fifo) {
        return 0;
    }
    if (rd->fifo->write_seq <= rd->read_seq) {
        return 0;
    }
    return (size_t)(rd->fifo->write_seq - rd->read_seq);
}
