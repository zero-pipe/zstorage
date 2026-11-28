#ifndef ZCS_VOD_IO_READER_H
#define ZCS_VOD_IO_READER_H

/**
 * @file reader.h
 * @brief 通用点播文件读取接口：磁盘容器 → vod_buffer。
 *
 * VOD 扩展边界。当前 MP4/MOV/MKV/FLV 由 file_reader 支撑；
 * 新文件容器应在此附着，勿将容器专用 reader 名泄漏到 play 代码。
 */
#include "zcs/core/stream/hub.h"
#include "zcs/core/stream/buffer.h"
#include "zcs/zcs_export.h"
#include "zn/poller/poller.h"
#include <stddef.h>
#include <stdint.h>

struct zcs_vod_flv_index;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_vod_reader zcs_vod_reader;

typedef struct zcs_vod_reader_opts {
    const char *file_path;
    const char *app;
    const char *stream;
    zcs_vod_buffer *fifo;
    zcs_media_source *source;
    double speed;
    int loop;
    /** 未消费 fifo 深度超过此值时暂停 demux。 */
    size_t fifo_high_water;
} zcs_vod_reader_opts;

/**
 * 打开一个文件点播读取接口（内部委托 file_reader，按扩展名选容器 demux）。
 * opts 中的 app/stream/fifo/source 等原样透传；内部 object 归接口所有。
 * @return 成功返回 reader；参数为空或底层打开失败返回 NULL（用 close 释放）。
 */
ZCS_API zcs_vod_reader *zcs_vod_reader_open(const zcs_vod_reader_opts *opts);
/** 关闭并释放接口及底层 file_reader；r 可为 NULL。调用后 r 不得再用。 */
ZCS_API void zcs_vod_reader_close(zcs_vod_reader *r);
/** 绑定 poller：之后由 poller 定时驱动 demux（而非同步 pump）。 */
ZCS_API void zcs_vod_reader_bind_poller(zcs_vod_reader *r, zn_poller *poller);
/** 绑定 poller 并做有限预读（每 play lane 一次，避免深栈 pump）。 */
ZCS_API void zcs_vod_reader_bind_poller_lite(zcs_vod_reader *r, zn_poller *poller);
/** open 后同步预读帧到 vod_buffer，供即时起播。 */
ZCS_API void zcs_vod_reader_prefill(zcs_vod_reader *r);
/** 重置 fifo，从文件头扫描到首个可解码 sync 帧并预填（PLAY 起播准备）。 */
ZCS_API void zcs_vod_reader_prepare_play(zcs_vod_reader *r);
/**
 * 按媒体时间 seek(ms)：重置 fifo 并预填到 sync 关键帧。
 * @return 实际落点位置（ms）。
 */
ZCS_API uint64_t zcs_vod_reader_seek_ms(zcs_vod_reader *r, uint64_t ms);
/** 取接口对应的媒体源（借用，不转移所有权）。 */
ZCS_API zcs_media_source *zcs_vod_reader_source(zcs_vod_reader *r);
/** 打开时的磁盘路径；无则返回 NULL */
ZCS_API const char *zcs_vod_reader_file_path(const zcs_vod_reader *r);
/** 点播总时长（ms）；未知返回 0。 */
ZCS_API uint64_t zcs_vod_reader_duration_ms(const zcs_vod_reader *r);
/** 取 FLV 文件索引（仅 FLV 容器有）；无则返回 NULL，指针借用。 */
ZCS_API const struct zcs_vod_flv_index *zcs_vod_reader_flv_index(zcs_vod_reader *r);
/** 暂停/恢复 demux 定时器（RTMP 背压或播放器暂停）；hold 非 0 即暂停。 */
ZCS_API void zcs_vod_reader_set_pump_hold(zcs_vod_reader *r, int hold);
/** 无 poller 时同步推进一次 demux（如构建 FLV 索引）；返回底层 pump 结果。 */
ZCS_API int zcs_vod_reader_pump(zcs_vod_reader *r);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_IO_READER_H */
