#ifndef ZCS_EGRESS_VOD_PLAY_PORT_H
#define ZCS_EGRESS_VOD_PLAY_PORT_H

/**
 * @file vod_play_port.h
 * @brief 出站层对点播 demux/lane 的最小依赖。
 *
 * egress 不包含 service 头；本文件声明出站泵需要的 VOD 能力，
 * 实现仍在 vod/io/reader.c 与 vod/play/play_lane.c。
 */
#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_vod_reader zcs_vod_reader;
typedef struct zcs_vod_play_lane zcs_vod_play_lane;

/**
 * 驱动一次点播文件读取，把解封装出的 sample 泵入 lane 的 fifo。
 * @return 本次泵入的帧数（<=0 表示无进展/已到尾声）
 */
ZCS_API int zcs_vod_reader_pump(zcs_vod_reader *r);

/** 取该 lane 关联的文件读取器；@return 借用指针，lane 无效返回 NULL。 */
ZCS_API zcs_vod_reader *zcs_vod_play_lane_reader(zcs_vod_play_lane *lane);
/** 设置/清除泵送暂停（背压时暂停继续读文件）。 */
ZCS_API void zcs_vod_play_lane_set_pump_hold(zcs_vod_play_lane *lane, int hold);
/**
 * 反复 pump 直到 fifo 积压达到目标窗口或达 max_pumps 上限。
 * 用于起播前预填缓冲，保证播放端有足够数据。
 */
ZCS_API void zcs_vod_play_lane_demux_fill(zcs_vod_play_lane *lane, int max_pumps);
/** 取点播视频参数集；@return 借用指针（归属 lane 的 fifo），无配置返回 NULL。 */
ZCS_API const uint8_t *zcs_vod_play_lane_video_config(const zcs_vod_play_lane *lane, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_VOD_PLAY_PORT_H */
