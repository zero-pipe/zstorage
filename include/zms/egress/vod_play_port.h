#ifndef ZMS_EGRESS_VOD_PLAY_PORT_H
#define ZMS_EGRESS_VOD_PLAY_PORT_H

/**
 * @file vod_play_port.h
 * @brief 出站层对点播 demux/lane 的最小依赖。
 *
 * egress 不包含 service 头；本文件声明出站泵需要的 VOD 能力，
 * 实现仍在 vod/io/reader.c 与 vod/play/play_lane.c。
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_reader zms_vod_reader;
typedef struct zms_vod_play_lane zms_vod_play_lane;

/**
 * 驱动一次点播文件读取，把解封装出的 sample 泵入 lane 的 fifo。
 * @return 本次泵入的帧数（<=0 表示无进展/已到尾声）
 */
ZMS_API int zms_vod_reader_pump(zms_vod_reader *r);

/** 取该 lane 关联的文件读取器；@return 借用指针，lane 无效返回 NULL。 */
ZMS_API zms_vod_reader *zms_vod_play_lane_reader(zms_vod_play_lane *lane);
/** 设置/清除泵送暂停（背压时暂停继续读文件）。 */
ZMS_API void zms_vod_play_lane_set_pump_hold(zms_vod_play_lane *lane, int hold);
/**
 * 反复 pump 直到 fifo 积压达到目标窗口或达 max_pumps 上限。
 * 用于起播前预填缓冲，保证播放端有足够数据。
 */
ZMS_API void zms_vod_play_lane_demux_fill(zms_vod_play_lane *lane, int max_pumps);
/** 取点播视频参数集；@return 借用指针（归属 lane 的 fifo），无配置返回 NULL。 */
ZMS_API const uint8_t *zms_vod_play_lane_video_config(const zms_vod_play_lane *lane, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_VOD_PLAY_PORT_H */
