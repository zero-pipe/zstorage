#ifndef ZMS_VOD_IO_FILE_READER_H
#define ZMS_VOD_IO_FILE_READER_H

/**
 * @file file_reader.h
 * @brief 文件 VOD demuxer：MP4/MOV/MKV/FLV → vod_buffer
 *
 * 不使用 channel 或 gop_queue。MP4 sample DTS 已是相对毫秒，绕过直播 media_timeline。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/core/stream/hub.h"
#include "zms/core/stream/buffer.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include "zn/poller/poller.h"
#include <stddef.h>

struct zms_vod_flv_index;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_file_reader zms_vod_file_reader;

typedef struct zms_vod_file_reader_opts {
    const char *file_path;
    const char *app;
    const char *stream;
    zms_vod_buffer *fifo;
    zms_media_source *source;
    double speed;
    int loop;
    /** 未消费 fifo 深度超过此值时暂停 demux。 */
    size_t fifo_high_water;
    /** 可选接口所有者，存入 zms_media_source::publisher_ctx。 */
    void *owner_ctx;
} zms_vod_file_reader_opts;

ZMS_LOCAL zms_vod_file_reader *zms_vod_file_reader_open(const zms_vod_file_reader_opts *opts);
ZMS_LOCAL void zms_vod_file_reader_close(zms_vod_file_reader *r);
ZMS_LOCAL void zms_vod_file_reader_bind_poller(zms_vod_file_reader *r, zn_poller *poller);

/** 绑定 poller 并有限 prefill（每 play lane，避免深栈 pump）。 */
ZMS_LOCAL void zms_vod_file_reader_bind_poller_lite(zms_vod_file_reader *r, zn_poller *poller);

/** open 后同步预读帧到 vod_buffer。 */
ZMS_LOCAL void zms_vod_file_reader_prefill(zms_vod_file_reader *r);

/** 重置 fifo，从文件头扫描到首个可解码 sync 帧并 prefill。 */
ZMS_LOCAL void zms_vod_file_reader_prepare_play(zms_vod_file_reader *r);

/** 按媒体毫秒 seek；重置 fifo 并 prefill 到 sync 关键帧；返回实际位置。 */
ZMS_LOCAL uint64_t zms_vod_file_reader_seek_ms(zms_vod_file_reader *r, uint64_t ms);
ZMS_LOCAL zms_media_source *zms_vod_file_reader_source(zms_vod_file_reader *r);
/** 打开时的磁盘路径；无则返回 NULL */
ZMS_LOCAL const char *zms_vod_file_reader_file_path(const zms_vod_file_reader *r);
ZMS_LOCAL uint64_t zms_vod_file_reader_duration_ms(const zms_vod_file_reader *r);
ZMS_LOCAL const struct zms_vod_flv_index *zms_vod_file_reader_flv_index(zms_vod_file_reader *r);

/** 暂停/恢复 demux 定时器（RTMP 背压或播放器暂停）。 */
ZMS_LOCAL void zms_vod_file_reader_set_pump_hold(zms_vod_file_reader *r, int hold);

/** 无 poller 时同步推进 demux（FLV 索引构建等）。 */
ZMS_LOCAL int zms_vod_file_reader_pump(zms_vod_file_reader *r);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_IO_FILE_READER_H */
