/**
 * @file play_lane.h
 * @brief VOD play lane。
 */
#ifndef ZMS_VOD_PLAY_PLAY_LANE_H
#define ZMS_VOD_PLAY_PLAY_LANE_H

/**
 * 单观众点播 lane：独占 vod_reader + vod_buffer，互不干扰。
 * RTSP 每会话一条 lane；媒体源 publisher 仍用共享 vod_buffer 做注册元数据。
 */
#include "zms/core/stream/hub.h"
#include "zms/core/stream/buffer.h"
#include "zms/vod/io/reader.h"
#include "zms/zms_export.h"
#include "zn/poller/poller.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_play_lane zms_vod_play_lane;

/** @param poller 可为 NULL（仅同步 pump，用于阻塞线程上的 FLV cache） */
ZMS_API zms_vod_play_lane *zms_vod_play_lane_open(const zms_media_source *src, zn_poller *poller);
/** 关闭并释放 lane（解绑 pub_hold、detach fifo reader、销毁 reader/buffer）；对 NULL 安全。 */
ZMS_API void zms_vod_play_lane_close(zms_vod_play_lane *lane);

/** 取 lane 内部 fifo 的输出 reader（借用，随 lane 关闭失效），供出站消费。 */
ZMS_API zms_vod_buffer_reader *zms_vod_play_lane_buffer_reader(zms_vod_play_lane *lane);
/** 取 lane 的 demux reader（借用，随 lane 关闭失效）。 */
ZMS_API zms_vod_reader *zms_vod_play_lane_reader(zms_vod_play_lane *lane);
/** seek 到 ms 并把 fifo reader 重新对齐到首个视频关键帧；返回实际落点毫秒（reader 为空返回 0）。 */
ZMS_API uint64_t zms_vod_play_lane_seek_ms(zms_vod_play_lane *lane, uint64_t ms);
/** 起播准备：等价于 seek_ms(start_ms)，用于确定初始定位。 */
ZMS_API uint64_t zms_vod_play_lane_prepare(zms_vod_play_lane *lane, uint64_t start_ms);

/** 从 reader 预取数据填充 fifo（受 high_water 上限约束），用于快速起步。 */
ZMS_API void zms_vod_play_lane_prefill(zms_vod_play_lane *lane);
/** prefill 后重新对齐 fifo reader 到首个视频关键帧 */
ZMS_API void zms_vod_play_lane_align_reader(zms_vod_play_lane *lane);
/** 暂停(hold 非 0)/恢复(hold=0) reader 的 pump，用于出站排空期间控流；不改变 buffer 已缓冲数据。 */
ZMS_API void zms_vod_play_lane_set_pump_hold(zms_vod_play_lane *lane, int hold);
/** RTSP/RTMP 出站排空期间持续填充 vod_buffer（不遵守 pump_hold）。 */
ZMS_API void zms_vod_play_lane_demux_fill(zms_vod_play_lane *lane, int max_pumps);
/** 取 fifo 上的视频配置参数集（借用指针，随 lane 关闭失效）；未就绪返回 NULL，len 输出长度。 */
ZMS_API const uint8_t *zms_vod_play_lane_video_config(const zms_vod_play_lane *lane, size_t *len);
/** 取 fifo 上的音频配置（借用指针，随 lane 关闭失效）；未就绪返回 NULL，len 输出长度。 */
ZMS_API const uint8_t *zms_vod_play_lane_audio_config(const zms_vod_play_lane *lane, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_PLAY_PLAY_LANE_H */
