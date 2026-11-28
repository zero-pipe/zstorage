/**
 * @file zm_file_demuxer.c
 * @brief 基于 ZM `zm_*_reader` 的文件 demuxer（MP4/MOV、MKV、FLV）。
 *
 * 把 `zm_mov_reader` / `zm_mkv_reader` / `zm_flv_reader` 适配成 ZCS 的
 * `zcs_container_demuxer_ops`，供 VOD 文件读取使用：
 *  - open_file 时把 `zm_track.config` 合成为 config 包；
 *  - pump 每次拉一个 `zm_packet`（视频已是 Annex-B）转成 sample 包；
 *  - seek 走接口的 `zm_reader_seek`。
 * MPEG 仍由旧路径处理。
 */
#include "zcs/media/container/container_registry.h"

#include "zcs/media/codec/codec_id.h"

#include "bridge/zm_file_io.h"
#include "media/container/mkv/mkv_file_buffer.h"
#include "media/container/mp4/mov_file_buffer.h"

#include "zm/core/reader.h"
#include "zm/core/packet.h"
#include "zm/core/track.h"
#include "zm/format/flv.h"
#include "zm/format/mkv.h"
#include "zm/format/mov.h"

#include "flv-proto.h"
#include "flv-reader.h"
#include "mkv-format.h"
#include "mkv-reader.h"
#include "mov-format.h"
#include "mov-reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* zm_codec_id 与 zcs_codec_id 在 1..10 上数值一致，可直接转换。 */

typedef struct zm_file_desc {
    zm_format format;
    int audio_config_always; /* MOV: 无 ASC 也发 audio config 包（保留默认 44.1k/2） */
    zm_reader *(*reader_open)(const zm_io *io, const zm_reader_cfg *cfg);
    uint64_t (*probe_duration_ms)(const char *path);
} zm_file_desc;

typedef struct zm_file_ctx {
    zcs_container_demux_opts cfg;
    const zm_file_desc *desc;
    zm_io io;
    int io_open;
    zm_reader *reader;
    char path[512];

    int has_video;
    int has_audio;
    uint32_t video_track;
    uint32_t audio_track;
    zcs_codec_id video_codec;
    zcs_codec_id audio_codec;

    uint64_t duration_ms;
    int eof;
} zm_file_ctx;

static uint64_t zcs_mkv_file_duration_ms(const char *path);

/* ---- config / track 载入 ---- */

static void file_emit_config(zm_file_ctx *c, uint32_t track, zcs_codec_id codec,
                             const uint8_t *data, size_t len, uint16_t width, uint16_t height,
                             uint32_t sample_rate, uint8_t channels)
{
    zcs_container_packet pkt;

    if (!c || !c->cfg.on_packet || codec == ZCS_CODEC_INVALID) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    /* 统一用 MP4_SAMPLE 种类承载 config：上层只按 codec + config 标志识别，
     * 不关心底层容器，简化了 MP4/MKV/FLV 三路上报。 */
    pkt.kind = ZCS_CONTAINER_PKT_MP4_SAMPLE;
    pkt.track_id = track;
    pkt.codec = codec;
    pkt.config = 1;
    pkt.width = width;
    pkt.height = height;
    pkt.sample_rate = sample_rate;
    pkt.channels = channels;
    pkt.data = data;
    pkt.len = len;
    c->cfg.on_packet(&pkt, c->cfg.user);
}

static void file_load_tracks(zm_file_ctx *c)
{
    const zm_track *tracks;
    size_t count = 0;
    size_t i;

    tracks = zm_reader_tracks(c->reader, &count);
    if (!tracks) {
        return;
    }
    /* 读取器只给出 track 元信息；上层需要的“config 包（SPS/PPS、ASC）”要在
     * 这里从 track->config 合成并通过 on_packet 主动发一次，否则下游拿不到
     * 解码参数。视频只处理第一个匹配到的轨（单视频流假设）。 */
    for (i = 0; i < count; ++i) {
        const zm_track *t = &tracks[i];
        zcs_codec_id codec = (zcs_codec_id)t->codec;
        uint32_t rate = t->sample_rate > 0 ? (uint32_t)t->sample_rate : 0;
        uint8_t ch = t->channels > 0 ? (uint8_t)t->channels : 0;

        if (t->kind == ZM_KIND_VIDEO) {
            c->video_track = t->track_id;
            c->video_codec = codec;
            c->has_video = 1;
            if (t->config.len > 0 && t->config.data) {
                file_emit_config(c, t->track_id, codec, t->config.data, t->config.len,
                                 (uint16_t)(t->width > 0 ? t->width : 0),
                                 (uint16_t)(t->height > 0 ? t->height : 0), 0, 0);
            }
        } else if (t->kind == ZM_KIND_AUDIO) {
            c->audio_track = t->track_id;
            c->audio_codec = codec;
            c->has_audio = 1;
            /* MOV 里 AAC 可能没有 ASC（stsd 缺 esds）：此时若 audio_config_always，
             * 仍发一个空 config 包，让下游用默认 44.1k/2 兜底，避免整条音轨被丢。 */
            if ((c->desc->audio_config_always && codec == ZCS_CODEC_AAC) || t->config.len > 0) {
                file_emit_config(c, t->track_id, codec, t->config.data, t->config.len, 0, 0, rate,
                                 ch);
            }
        }
    }
}

/* ---- 文件 I/O 生命周期 ---- */

static void file_close(zm_file_ctx *c)
{
    if (!c) {
        return;
    }
    /* 幂等关闭：reader 先关再关 io（reader 依赖 io）。把所有“已加载”标志
     * 复位，使同一 ctx 可以被 file_open 复用（open 开头会先 file_close）。 */
    if (c->reader) {
        zm_reader_close(c->reader);
        c->reader = NULL;
    }
    if (c->io_open) {
        zcs_zm_file_io_close(&c->io);
        c->io_open = 0;
    }
    c->has_video = 0;
    c->has_audio = 0;
    c->video_track = 0;
    c->audio_track = 0;
    c->video_codec = ZCS_CODEC_INVALID;
    c->audio_codec = ZCS_CODEC_INVALID;
    c->duration_ms = 0;
    c->eof = 0;
}

static zn_err_t file_open(void *ctx, const char *path)
{
    zm_file_ctx *c = (zm_file_ctx *)ctx;
    zm_reader_cfg rcfg;

    if (!c || !path || !path[0]) {
        return ZN_ERR_INVALID;
    }
    file_close(c);
    if ((size_t)snprintf(c->path, sizeof(c->path), "%s", path) >= sizeof(c->path)) {
        return ZN_ERR_INVALID;
    }
    if (zcs_zm_file_io_open(path, &c->io) != 0) {
        return ZN_ERR_IO;
    }
    c->io_open = 1;

    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.format = c->desc->format;
    /* ZM_READ_ES：让读取器直接把视频转成 Annex-B ES 输出（而非封装帧），
     * 与上层“拿到即可喂 PS/FLV”的约定一致。 */
    rcfg.mode = ZM_READ_ES;
    c->reader = c->desc->reader_open(&c->io, &rcfg);
    if (!c->reader) {
        file_close(c);
        return ZN_ERR_IO;
    }

    file_load_tracks(c);
    c->duration_ms = c->desc->probe_duration_ms ? c->desc->probe_duration_ms(path) : 0;
    /* 只支持 H.264/H.265 直出：本 demuxer 承诺“视频已是 Annex-B”，其它编码
     * （如 AV1/VP9 在 MP4 里）格式不满足上层对 Annex-B 的假设，早失败好过
     * 让脏数据流到下游。 */
    if (!c->has_video || (c->video_codec != ZCS_CODEC_H264 && c->video_codec != ZCS_CODEC_H265)) {
        file_close(c);
        return ZN_ERR_NOT_IMPL;
    }
    c->eof = 0;
    return ZN_OK;
}

static void file_close_op(void *ctx)
{
    file_close((zm_file_ctx *)ctx);
}

static int file_pump(void *ctx)
{
    zm_file_ctx *c = (zm_file_ctx *)ctx;
    zm_packet pkt;
    int rc;

    /* 返回值语义（供上层调度）：1=产出/推进了一步，0=到达 EOF，-1=读错误。
     * 上层据此决定继续轮询、结束播放还是报错。 */
    if (!c || !c->reader || c->eof) {
        return 0;
    }
    zm_packet_init(&pkt);
    rc = zm_reader_read(c->reader, &pkt);
    if (rc <= 0) {
        zm_packet_clear(&pkt);
        /* ZM 约定：read 返回 0 = 文件结束，<0 = 出错。eof 后不再重读，
         * 让上层把该源当成“播完”而非持续空转。 */
        if (rc == 0) {
            c->eof = 1;
            return 0;
        }
        return -1;
    }
    if (c->cfg.on_packet && pkt.mem.data && pkt.mem.len > 0) {
        zcs_codec_id codec = ZCS_CODEC_INVALID;

        if (c->has_video && pkt.track_id == c->video_track) {
            codec = c->video_codec;
        } else if (c->has_audio && pkt.track_id == c->audio_track) {
            codec = c->audio_codec;
        }
        if (codec != ZCS_CODEC_INVALID) {
            zcs_container_packet out;

            memset(&out, 0, sizeof(out));
            out.kind = ZCS_CONTAINER_PKT_MP4_SAMPLE;
            out.track_id = pkt.track_id;
            out.codec = codec;
            /* pts 缺省（-1/0）时退回 dts：容器里偶有 packet 无 pts，直接留 0
             * 会让下游时序错乱（时间倒退），退回 dts 保证单调。 */
            out.pts_ms = (uint32_t)(pkt.pts_ms > 0 ? pkt.pts_ms : pkt.dts_ms);
            out.dts_ms = (uint32_t)pkt.dts_ms;
            out.key = (pkt.flags & ZM_PKT_KEY) ? 1 : 0;
            out.data = pkt.mem.data;
            out.len = pkt.mem.len;
            c->cfg.on_packet(&out, c->cfg.user);
        }
    }
    zm_packet_clear(&pkt);
    /* 被过滤的轨道也计一次推进，匹配旧的 pump 语义（loop / 背压依赖）。 */
    return 1;
}

static zn_err_t file_seek(void *ctx, int64_t *seek_ms)
{
    zm_file_ctx *c = (zm_file_ctx *)ctx;
    int64_t ts;

    if (!c || !c->reader || !seek_ms) {
        return ZN_ERR_INVALID;
    }
    ts = *seek_ms;
    /* 以入参为“期望位置”，reader 可能吸附到最近关键帧：把实际落点回写
     * 给 *seek_ms，让上层知道真正跳到了哪里。seek 隐含清 eof，允许续读。 */
    if (zm_reader_seek(c->reader, &ts) != ZM_OK) {
        return ZN_ERR_INVALID;
    }
    *seek_ms = ts;
    c->eof = 0;
    return ZN_OK;
}

static uint64_t file_duration_ms(void *ctx)
{
    zm_file_ctx *c = (zm_file_ctx *)ctx;

    return c ? c->duration_ms : 0;
}

static zn_err_t file_feed(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;
    (void)buf;
    (void)len;
    return ZN_ERR_INVALID;
}

static zn_err_t file_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                                uint32_t tag_dts_ms)
{
    (void)ctx;
    (void)type_id;
    (void)body;
    (void)len;
    (void)tag_dts_ms;
    return ZN_ERR_INVALID;
}

/* ---- 描述符 / vtable ---- */

static const zm_file_desc *file_desc_for(zcs_container_id id)
{
    /* 每种容器绑定：ZM 读取器工厂 + 时长探测函数；audio_config_always 仅
     * MOV 置 1（见 file_load_tracks 注释）。MPEG 不走这里，由旧路径处理。 */
    static const zm_file_desc k_mov = {ZM_FMT_MOV, 1, zm_mov_reader_open,
                                        zcs_container_mp4_file_duration_ms};
    static const zm_file_desc k_mkv = {ZM_FMT_MKV, 0, zm_mkv_reader_open,
                                        zcs_mkv_file_duration_ms};
    static const zm_file_desc k_flv = {ZM_FMT_FLV, 0, zm_flv_reader_open_file,
                                        zcs_container_flv_file_duration_ms};

    if (id == ZCS_CONTAINER_MP4) {
        return &k_mov;
    }
    if (id == ZCS_CONTAINER_MKV) {
        return &k_mkv;
    }
    if (id == ZCS_CONTAINER_FLV_FILE) {
        return &k_flv;
    }
    return NULL;
}

static void *file_create(const zcs_container_demux_opts *opts)
{
    zm_file_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (zm_file_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    c->desc = file_desc_for(opts->id);
    if (!c->desc) {
        free(c);
        return NULL;
    }
    c->video_codec = ZCS_CODEC_INVALID;
    c->audio_codec = ZCS_CODEC_INVALID;
    return c;
}

static void file_destroy(void *ctx)
{
    zm_file_ctx *c = (zm_file_ctx *)ctx;

    if (!c) {
        return;
    }
    file_close(c);
    free(c);
}

const zcs_container_demuxer_ops zcs_container_mp4_ops = {
    ZCS_CONTAINER_MP4, "mp4",         file_create,    file_destroy,
    file_feed,         file_input_tag, file_open,     file_close_op,
    file_pump,         file_seek,     file_duration_ms,
};

const zcs_container_demuxer_ops zcs_container_mkv_ops = {
    ZCS_CONTAINER_MKV, "mkv",         file_create,    file_destroy,
    file_feed,         file_input_tag, file_open,     file_close_op,
    file_pump,         file_seek,     file_duration_ms,
};

const zcs_container_demuxer_ops zcs_container_flv_file_ops = {
    ZCS_CONTAINER_FLV_FILE, "flv-file",    file_create,    file_destroy,
    file_feed,              file_input_tag, file_open,     file_close_op,
    file_pump,              file_seek,     file_duration_ms,
};

static void file_codecs(void *ctx, zcs_codec_id *video, zcs_codec_id *audio)
{
    zm_file_ctx *c = (zm_file_ctx *)ctx;

    if (video) {
        *video = c ? c->video_codec : ZCS_CODEC_INVALID;
    }
    if (audio) {
        *audio = c ? c->audio_codec : ZCS_CODEC_INVALID;
    }
}

void zcs_container_demux_mp4_codecs(void *ctx, zcs_codec_id *video, zcs_codec_id *audio)
{
    file_codecs(ctx, video, audio);
}

void zcs_container_demux_mkv_codecs(void *ctx, zcs_codec_id *video, zcs_codec_id *audio)
{
    file_codecs(ctx, video, audio);
}

void zcs_container_demux_flv_file_codecs(void *ctx, zcs_codec_id *video, zcs_codec_id *audio)
{
    file_codecs(ctx, video, audio);
}

/* ---- MP4 专用：sample 表遍历 / duration 探测 ---- */

struct mov_reader_t *zcs_container_mp4_mov_reader(void *ctx)
{
    /* ZM reader 不再暴露内部 mov_reader_t；保留符号以维持 ABI。 */
    (void)ctx;
    return NULL;
}

typedef struct {
    zcs_mp4_sample_fn fn;
    void *user;
} mp4_sample_forward;

static int mp4_on_sample(void *param, const struct mov_reader_sample_t *s)
{
    mp4_sample_forward *f = (mp4_sample_forward *)param;
    zcs_mp4_sample_info info;

    info.dts_ms = s->dts;
    info.bytes = s->bytes;
    info.is_video = s->is_video;
    info.key = s->key;
    return f->fn(&info, f->user);
}

int zcs_container_mp4_for_each_sample(void *ctx, zcs_mp4_sample_fn fn, void *user)
{
    zm_file_ctx *c = (zm_file_ctx *)ctx;
    mp4_sample_forward f;
    struct mov_reader_t *reader;
    FILE *fp;
    int rc;

    if (!c || !c->path[0] || !fn) {
        return -1;
    }
    /* 遍历 sample 表另开一个 mov_reader：ZM reader 不对外暴露内部
     * mov_reader_t，也无法在 ES 读取器上直接遍历 sample 索引，所以这里
     * 独立 fopen 一个只读句柄走底层 API，遍历完立即关闭，不影响主 reader。 */
    fp = fopen(c->path, "rb");
    if (!fp) {
        return -1;
    }
    reader = mov_reader_create(zcs_mov_file_buffer(), fp);
    if (!reader) {
        fclose(fp);
        return -1;
    }
    f.fn = fn;
    f.user = user;
    rc = mov_reader_foreach_sample(reader, mp4_on_sample, &f);
    mov_reader_destroy(reader);
    fclose(fp);
    return rc;
}

typedef struct {
    uint8_t video_object;
    enum mkv_codec_t mkv_video;
} probe_track;

static void probe_mov_onvideo(void *param, uint32_t track, uint8_t object, int width, int height,
                              const void *extra, size_t bytes)
{
    probe_track *p = (probe_track *)param;

    (void)track;
    (void)width;
    (void)height;
    (void)extra;
    (void)bytes;
    if (p) {
        p->video_object = object;
    }
}

uint64_t zcs_container_mp4_file_duration_ms(const char *path)
{
    struct mov_reader_trackinfo_t info;
    struct mov_reader_t *reader;
    probe_track pt;
    FILE *fp;
    uint64_t dur = 0;

    if (!path || !path[0]) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    reader = mov_reader_create(zcs_mov_file_buffer(), fp);
    if (!reader) {
        fclose(fp);
        return 0;
    }
    memset(&pt, 0, sizeof(pt));
    memset(&info, 0, sizeof(info));
    info.onvideo = probe_mov_onvideo;
    /* 只有视频确实是 H.264/H.265 时才信 duration：与 file_open 的编码校验
     * 保持一致，否则会给一个上层根本不会播的文件报非零时长。 */
    if (mov_reader_getinfo(reader, &info, &pt) == 0 &&
        (pt.video_object == MOV_OBJECT_H264 || pt.video_object == MOV_OBJECT_H265)) {
        dur = mov_reader_getduration(reader);
    }
    mov_reader_destroy(reader);
    fclose(fp);
    return dur;
}

static void probe_mkv_onvideo(void *param, uint32_t track, enum mkv_codec_t codec, int width,
                              int height, const void *extra, size_t bytes)
{
    probe_track *p = (probe_track *)param;

    (void)track;
    (void)width;
    (void)height;
    (void)extra;
    (void)bytes;
    if (p) {
        p->mkv_video = codec;
    }
}

static uint64_t zcs_mkv_file_duration_ms(const char *path)
{
    struct mkv_reader_trackinfo_t info;
    struct mkv_reader_t *reader;
    probe_track pt;
    FILE *fp;
    uint64_t dur = 0;

    if (!path || !path[0]) {
        return 0;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    reader = mkv_reader_create(zcs_mkv_file_buffer(), fp);
    if (!reader) {
        fclose(fp);
        return 0;
    }
    memset(&pt, 0, sizeof(pt));
    memset(&info, 0, sizeof(info));
    info.onvideo = probe_mkv_onvideo;
    if (mkv_reader_getinfo(reader, &info, &pt) == 0 &&
        (pt.mkv_video == MKV_CODEC_VIDEO_H264 || pt.mkv_video == MKV_CODEC_VIDEO_H265)) {
        dur = mkv_reader_getduration(reader);
    }
    mkv_reader_destroy(reader);
    fclose(fp);
    return dur;
}

uint64_t zcs_container_flv_file_duration_ms(const char *path)
{
    void *reader;
    int tagtype;
    uint32_t ts;
    size_t taglen;
    uint8_t buf[64 * 1024];
    uint64_t max_ts = 0;
    int n;

    if (!path || !path[0]) {
        return 0;
    }
    reader = flv_reader_create(path);
    if (!reader) {
        return 0;
    }
    /* FLV 没有全局时长字段：只能顺序读完全部 tag，取最大的音视频时间戳
     * 作为时长。用固定 64KB 栈缓冲，单个 tag 超过它就截断退出（异常文件
     * 大概率也不该被当作正常点播源）。 */
    for (;;) {
        tagtype = 0;
        ts = 0;
        taglen = 0;
        n = flv_reader_read(reader, &tagtype, &ts, &taglen, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        if (taglen > sizeof(buf)) {
            break;
        }
        if ((tagtype == FLV_TYPE_AUDIO || tagtype == FLV_TYPE_VIDEO) && (uint64_t)ts > max_ts) {
            max_ts = ts;
        }
    }
    flv_reader_destroy(reader);
    return max_ts;
}
