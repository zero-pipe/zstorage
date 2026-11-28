/**
 * @file zs_slot_demuxer.c
 * @brief `.zslot` 容器 demuxer：把槽位数据区当 MPEG-PS 文件读。
 *
 * 槽位文件不是连续 PS 流：Zone A/B 是头和索引，Zone C 才是 PS 数据区，
 * 且文件末尾是预分配的空洞。所以不能直接交给文件 demuxer，这里：
 *   - open_file 解析槽位头，拿到 data_start/data_write_off；
 *   - pump 按块读数据区喂给现成的 zcs_mpegps_demuxer；
 *   - seek 用槽位关键帧索引定位到"不晚于目标时间的最近关键帧"；
 *   - duration_ms 由索引首尾关键帧时间差给出。
 *
 * 输出统一成 ZCS_CONTAINER_PKT_MP4_SAMPLE，与 MP4/MKV/FLV 文件走同一条
 * VOD 路径，file_reader / FLV / HLS / RTSP 都无需改动。
 */
#include "zcs/media/container/container_registry.h"
#include "zcs/media/container/mpegps/mpegps_demuxer.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/codec/h264/h264_over_rtmp.h"
#include "storage/zs_slot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZS_DEMUX_CHUNK (192u * 1024u)
#define ZS_DEMUX_TRACK_VIDEO 1u
#define ZS_DEMUX_TRACK_AUDIO 2u

typedef struct zs_slot_ctx {
    zcs_container_demux_opts cfg;
    zs_slot *slot;
    FILE *fp;

    zcs_mpegps_demuxer *ps;
    int ps_open;

    uint64_t read_off; /* 下一个待读的数据区偏移 */
    uint64_t data_end; /* 数据区有效终点（open 时快照） */
    uint64_t first_kf_ms;
    uint64_t last_kf_ms;
    uint64_t next_idx_utc; /* 下一个关键帧时间（用于给样本打 key 标志） */

    int has_video;
    int has_audio;
    zcs_codec_id video_codec;
    zcs_codec_id audio_codec;
    uint32_t width;
    uint32_t height;

    int eof;
    uint8_t buf[ZS_DEMUX_CHUNK];
} zs_slot_ctx;

/* ── PS 解出的帧 -> 容器包 ──────────────────────────────────────────────── */

static void zs_demux_emit_config(zs_slot_ctx *c, zcs_codec_id codec, const uint8_t *data,
                                 size_t len, uint32_t sample_rate, uint32_t channels)
{
    zcs_container_packet pkt;

    if (!c->cfg.on_packet || !data || len == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZCS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.codec = codec;
    pkt.config = 1;
    pkt.track_id = codec == ZCS_CODEC_AAC ? ZS_DEMUX_TRACK_AUDIO : ZS_DEMUX_TRACK_VIDEO;
    pkt.data = data;
    pkt.len = len;
    pkt.width = (uint16_t)c->width;
    pkt.height = (uint16_t)c->height;
    pkt.sample_rate = sample_rate;
    pkt.channels = (uint8_t)channels;
    c->cfg.on_packet(&pkt, c->cfg.user);
}

/* 从帧内联参数集合成 FLV 风格 sequence header（与 MP4 路径给 file_reader 的一致）。 */
static void zs_demux_try_video_config(zs_slot_ctx *c, const uint8_t *data, size_t len)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;

    if (c->video_codec == ZCS_CODEC_H264) {
        size_t sps_len = 0;
        size_t pps_len = 0;
        uint8_t out[768];
        size_t out_len = 0;

        if (!zcs_h264_annexb_extract_sps_pps(data, len, &sps, &sps_len, &pps, &pps_len)) {
            return;
        }
        if (zcs_rtmp_avc_seq_header(sps, sps_len, pps, pps_len, out, sizeof(out), &out_len) !=
            ZN_OK) {
            return;
        }
        /* file_reader 的视频配置契约是“裸” AVCDecoderConfigurationRecord（以 avcC 的
         * configurationVersion=1 开头），它自己负责包 FLV 序列头。而
         * zcs_rtmp_avc_seq_header 返回的是“FLV 视频 tag 头(5B) + avcC”。这里剥掉前
         * 5 字节给裸 avcC，否则下游会再套一层头，SPS 偏移错位 -> 宽高 0x0 / No start code。 */
        if (out_len <= 5) {
            return;
        }
        zs_demux_emit_config(c, ZCS_CODEC_H264, out + 5, out_len - 5, 0, 0);
    } else if (c->video_codec == ZCS_CODEC_H265) {
        const uint8_t *vps = NULL;
        size_t vps_len = 0;
        size_t sps_len = 0;
        size_t pps_len = 0;
        uint8_t hvcc[1024];
        size_t hvcc_len = 0;

        if (!zcs_h265_annexb_extract_vps_sps_pps(data, len, &vps, &vps_len, &sps, &sps_len,
                                                 &pps, &pps_len)) {
            return;
        }
        if (zcs_h265_hvcc_from_annexb(data, len, hvcc, sizeof(hvcc), &hvcc_len) != ZN_OK ||
            hvcc_len == 0) {
            return;
        }
        zs_demux_emit_config(c, ZCS_CODEC_H265, hvcc, hvcc_len, 0, 0);
    }
}

/* 时间基准统一为「槽位内相对毫秒，从 0 起」：
 * PS 帧里是 UTC 绝对毫秒（uint32 取低 32 位），而 seek 参数、file_reader 的
 * duration / drop_until_sync、RTP 锚点、播放器进度条全部用段内相对毫秒。
 * 不在这里归一化，任何非 0 seek 都会把 reader 跳到约 1.6e7 ms 之外的位置，
 * fifo 永远填不进目标帧，出站泵就一直 lag 满 + peek 失败刷日志。 */
static uint32_t zs_demux_rel_ms(zs_slot_ctx *c, uint32_t ts_ms)
{
    if (!c || ts_ms == 0) {
        return 0;
    }
    return ts_ms > c->first_kf_ms ? ts_ms - (uint32_t)c->first_kf_ms : 0;
}

static void zs_demux_on_frame(const zcs_frame *frame, void *user)
{
    zs_slot_ctx *c = (zs_slot_ctx *)user;
    zcs_container_packet pkt;

    if (!c || !frame || !c->cfg.on_packet || !frame->data || frame->size == 0) {
        return;
    }
    if (frame->track == ZCS_TRACK_VIDEO && !c->has_video) {
        c->has_video = 1;
        c->video_codec = frame->codec;
    } else if (frame->track == ZCS_TRACK_AUDIO && !c->has_audio) {
        c->has_audio = 1;
        c->audio_codec = frame->codec;
    }
    if (frame->track == ZCS_TRACK_VIDEO && frame->keyframe) {
        zs_demux_try_video_config(c, frame->data, frame->size);
    }

    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZCS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.track_id = frame->track == ZCS_TRACK_AUDIO ? ZS_DEMUX_TRACK_AUDIO : ZS_DEMUX_TRACK_VIDEO;
    pkt.codec = frame->codec;
    pkt.pts_ms = zs_demux_rel_ms(c, (uint32_t)frame->pts_ms);
    pkt.dts_ms = zs_demux_rel_ms(c, frame->dts_ms ? (uint32_t)frame->dts_ms : (uint32_t)frame->pts_ms);
    pkt.key = frame->keyframe ? 1 : 0;
    pkt.data = frame->data;
    pkt.len = frame->size;
    c->cfg.on_packet(&pkt, c->cfg.user);
}

/* ── 槽位 -> PS 喂入 ────────────────────────────────────────────────────── */

static int zs_demux_feed_more(zs_slot_ctx *c)
{
    uint64_t remain;
    size_t want;

    if (!c->slot || c->read_off >= c->data_end) {
        c->eof = 1;
        return 0;
    }
    remain = c->data_end - c->read_off;
    want = (size_t)(remain < (uint64_t)ZS_DEMUX_CHUNK ? remain : ZS_DEMUX_CHUNK);
    if (zs_slot_pread(c->slot, c->buf, want, c->read_off) != 0) {
        return -1;
    }
    c->read_off += want;
    if (zcs_mpegps_demuxer_feed(c->ps, c->buf, want) != ZN_OK) {
        return -1;
    }
    return 1;
}

typedef struct zs_probe_ctx {
    zcs_codec_id video;
    zcs_codec_id audio;
    uint32_t width;
    uint32_t height;
} zs_probe_ctx;

static void zs_probe_on_frame(const zcs_frame *frame, void *user)
{
    zs_probe_ctx *pr = (zs_probe_ctx *)user;

    if (!pr || !frame) {
        return;
    }
    if (frame->track == ZCS_TRACK_VIDEO && pr->video == ZCS_CODEC_INVALID) {
        pr->video = frame->codec;
    } else if (frame->track == ZCS_TRACK_AUDIO && pr->audio == ZCS_CODEC_INVALID) {
        pr->audio = frame->codec;
    }
}

/*
 * 探测首个可解帧的 codec：file_reader 在 open 后立刻要 codec，而 PS 解出的
 * 帧要到第一块数据喂进去才有。这里用一次性 demuxer 读前几块拿到 codec，
 * 随后销毁重建，正式回放不会丢帧。
 */
static void zs_demux_probe_codecs(zs_slot_ctx *c)
{
    zcs_mpegps_demuxer_opts popts;
    zcs_mpegps_demuxer *probe;
    zs_probe_ctx pr;
    uint64_t off = zs_slot_data_start(c->slot);
    int rounds;

    memset(&pr, 0, sizeof(pr));
    pr.video = ZCS_CODEC_INVALID;
    pr.audio = ZCS_CODEC_INVALID;
    memset(&popts, 0, sizeof(popts));
    popts.on_frame = zs_probe_on_frame;
    popts.user = &pr;
    probe = zcs_mpegps_demuxer_create(&popts);
    if (!probe) {
        return;
    }
    for (rounds = 0; rounds < 8 && off < c->data_end; ++rounds) {
        uint64_t remain = c->data_end - off;
        size_t want = (size_t)(remain < (uint64_t)ZS_DEMUX_CHUNK ? remain : ZS_DEMUX_CHUNK);

        if (zs_slot_pread(c->slot, c->buf, want, off) != 0) {
            break;
        }
        off += want;
        (void)zcs_mpegps_demuxer_feed(probe, c->buf, want);
        if (pr.video != ZCS_CODEC_INVALID && pr.audio != ZCS_CODEC_INVALID) {
            break;
        }
    }
    zcs_mpegps_demuxer_destroy(probe);
    c->video_codec = pr.video;
    c->audio_codec = pr.audio;
}

/* ── vtable ─────────────────────────────────────────────────────────────── */

static void *zs_demux_create(const zcs_container_demux_opts *opts)
{
    zs_slot_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (zs_slot_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    return c;
}

static void zs_demux_close(zs_slot_ctx *c)
{
    if (c->ps) {
        zcs_mpegps_demuxer_destroy(c->ps);
        c->ps = NULL;
    }
    if (c->slot) {
        zs_slot_close(c->slot);
        c->slot = NULL;
    }
    c->ps_open = 0;
}

static void zs_demux_destroy(void *ctx)
{
    zs_slot_ctx *c = (zs_slot_ctx *)ctx;

    if (!c) {
        return;
    }
    zs_demux_close(c);
    free(c);
}

static zn_err_t zs_demux_open(void *ctx, const char *path)
{
    zs_slot_ctx *c = (zs_slot_ctx *)ctx;
    zs_keyframe_entry first;
    zs_keyframe_entry last;
    zcs_mpegps_demuxer_opts popts;
    uint32_t n;

    if (!c || !path || !path[0]) {
        return ZN_ERR_INVALID;
    }
    zs_demux_close(c);
    c->slot = zs_slot_open(path);
    if (!c->slot) {
        return ZN_ERR_IO;
    }
    n = zs_slot_keyframe_count(c->slot);
    if (n == 0 || zs_slot_get_keyframe(c->slot, 0, &first) != 0 ||
        zs_slot_get_keyframe(c->slot, n - 1u, &last) != 0) {
        zs_demux_close(c);
        return ZN_ERR_INVALID;
    }
    c->first_kf_ms = first.utc_ms;
    c->last_kf_ms = last.utc_ms;
    c->data_end = zs_slot_write_offset(c->slot);
    c->read_off = zs_slot_data_start(c->slot);
    c->next_idx_utc = first.utc_ms;
    c->eof = 0;
    c->has_video = 0;
    c->has_audio = 0;

    memset(&popts, 0, sizeof(popts));
    popts.on_frame = zs_demux_on_frame;
    popts.user = c;
    c->ps = zcs_mpegps_demuxer_create(&popts);
    if (!c->ps) {
        zs_demux_close(c);
        return ZN_ERR_NOMEM;
    }
    c->ps_open = 1;
    zs_demux_probe_codecs(c);
    return ZN_OK;
}

static void zs_demux_close_op(void *ctx)
{
    zs_demux_close((zs_slot_ctx *)ctx);
}

static int zs_demux_pump(void *ctx)
{
    zs_slot_ctx *c = (zs_slot_ctx *)ctx;
    int rc;

    if (!c || !c->slot || !c->ps_open || c->eof) {
        return 0;
    }
    /* 每次 pump 读一块并喂给 PS；有帧回调自然产生，无帧也返回 1（推进了一步）。 */
    rc = zs_demux_feed_more(c);
    if (rc <= 0) {
        return rc;
    }
    return 1;
}

/* 定位到 <= target 的最近关键帧，并把数据区读指针挪到该关键帧的 PS 包处。 */
static zn_err_t zs_demux_seek(void *ctx, int64_t *seek_ms)
{
    zs_slot_ctx *c = (zs_slot_ctx *)ctx;
    zs_keyframe_entry e;
    int64_t abs_ms;

    if (!c || !c->slot || !seek_ms) {
        return ZN_ERR_INVALID;
    }
    abs_ms = c->first_kf_ms + *seek_ms;
    if (abs_ms < (int64_t)c->first_kf_ms) {
        abs_ms = (int64_t)c->first_kf_ms;
    }
    if (zs_slot_find(c->slot, (uint64_t)abs_ms, &e) != 0) {
        return ZN_ERR_INVALID; /* 早于首个关键帧 */
    }
    c->read_off = e.file_offset;
    c->eof = 0;
    *seek_ms = (int64_t)(e.utc_ms - c->first_kf_ms);
    return ZN_OK;
}

static uint64_t zs_demux_duration(void *ctx)
{
    zs_slot_ctx *c = (zs_slot_ctx *)ctx;

    if (!c || !c->slot || c->last_kf_ms <= c->first_kf_ms) {
        return 0;
    }
    return c->last_kf_ms - c->first_kf_ms;
}

static zn_err_t zs_demux_feed(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return ZN_ERR_INVALID;
}

static zn_err_t zs_demux_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                                   uint32_t tag_dts_ms)
{
    (void)ctx;
    (void)type_id;
    (void)body;
    (void)len;
    (void)tag_dts_ms;
    return ZN_ERR_INVALID;
}

const zcs_container_demuxer_ops zcs_container_zslot_ops = {
    ZCS_CONTAINER_ZSLOT, "zslot",       zs_demux_create, zs_demux_destroy,
    zs_demux_feed,       zs_demux_input_tag, zs_demux_open, zs_demux_close_op,
    zs_demux_pump,       zs_demux_seek,      zs_demux_duration,
};

/* 供 file_reader 判断容器后取 codec。 */
void zcs_container_demux_zslot_codecs(void *ctx, zcs_codec_id *video, zcs_codec_id *audio)
{
    zs_slot_ctx *c = (zs_slot_ctx *)ctx;

    if (video) {
        *video = c ? c->video_codec : ZCS_CODEC_INVALID;
    }
    if (audio) {
        *audio = c ? c->audio_codec : ZCS_CODEC_INVALID;
    }
}
