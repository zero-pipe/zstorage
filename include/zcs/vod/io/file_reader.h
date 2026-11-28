#ifndef ZCS_VOD_IO_FILE_READER_H
#define ZCS_VOD_IO_FILE_READER_H

/**
 * @file file_reader.h
 * @brief 文件 VOD demuxer：MP4/MOV/MKV/FLV → vod_buffer
 *
 * 不使用 channel 或 gop_queue。MP4 sample DTS 已是相对毫秒，绕过直播 media_timeline。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/stream/hub.h"
#include "zcs/core/stream/buffer.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/poller/poller.h"
#include <stddef.h>

struct zcs_vod_flv_index;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_vod_file_reader zcs_vod_file_reader;

typedef struct zcs_vod_file_reader_opts {
    const char *file_path;
    const char *app;
    const char *stream;
    zcs_vod_buffer *fifo;
    zcs_media_source *source;
    double speed;
    int loop;
    /** 未消费 fifo 深度超过此值时暂停 demux。 */
    size_t fifo_high_water;
    /** 可选接口所有者，存入 zcs_media_source::publisher_ctx。 */
    void *owner_ctx;
} zcs_vod_file_reader_opts;

ZCS_LOCAL zcs_vod_file_reader *zcs_vod_file_reader_open(const zcs_vod_file_reader_opts *opts);
ZCS_LOCAL void zcs_vod_file_reader_close(zcs_vod_file_reader *r);
ZCS_LOCAL void zcs_vod_file_reader_bind_poller(zcs_vod_file_reader *r, zn_poller *poller);

/** 绑定 poller 并有限 prefill（每 play lane，避免深栈 pump）。 */
ZCS_LOCAL void zcs_vod_file_reader_bind_poller_lite(zcs_vod_file_reader *r, zn_poller *poller);

/** open 后同步预读帧到 vod_buffer。 */
ZCS_LOCAL void zcs_vod_file_reader_prefill(zcs_vod_file_reader *r);

/** 重置 fifo，从文件头扫描到首个可解码 sync 帧并 prefill。 */
ZCS_LOCAL void zcs_vod_file_reader_prepare_play(zcs_vod_file_reader *r);

/** 按媒体毫秒 seek；重置 fifo 并 prefill 到 sync 关键帧；返回实际位置。 */
ZCS_LOCAL uint64_t zcs_vod_file_reader_seek_ms(zcs_vod_file_reader *r, uint64_t ms);
ZCS_LOCAL zcs_media_source *zcs_vod_file_reader_source(zcs_vod_file_reader *r);
/** 打开时的磁盘路径；无则返回 NULL */
ZCS_LOCAL const char *zcs_vod_file_reader_file_path(const zcs_vod_file_reader *r);
ZCS_LOCAL uint64_t zcs_vod_file_reader_duration_ms(const zcs_vod_file_reader *r);
ZCS_LOCAL const struct zcs_vod_flv_index *zcs_vod_file_reader_flv_index(zcs_vod_file_reader *r);

/** 暂停/恢复 demux 定时器（RTMP 背压或播放器暂停）。 */
ZCS_LOCAL void zcs_vod_file_reader_set_pump_hold(zcs_vod_file_reader *r, int hold);

/** 无 poller 时同步推进 demux（FLV 索引构建等）。 */
ZCS_LOCAL int zcs_vod_file_reader_pump(zcs_vod_file_reader *r);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_IO_FILE_READER_H */
