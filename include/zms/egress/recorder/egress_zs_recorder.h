#ifndef ZMS_EGRESS_RECORDER_EGRESS_ZS_RECORDER_H
#define ZMS_EGRESS_RECORDER_EGRESS_ZS_RECORDER_H

/**
 * @file egress_zs_recorder.h
 * @brief 中心视频云存储录制器：GOP 队列 -> PS 复用 -> 预分配槽位。
 *
 * 与 egress_mp4_recorder 的区别：
 *   - 落盘是固定大小槽位 + 关键帧索引，断电可恢复（MP4 未封口则整段不可播）；
 *   - 每个 IO 周期攒包写盘，写入粒度是 PS pack，不重封容器；
 *   - 槽位可被回收复用，磁盘是"一次预分配、循环使用"。
 *
 * 生命周期：configure（进程启动一次）→ 推流时自动开录 → 断流时封口。
 *
 * 已知限制：
 *   - 音频是裸帧直入 PSM。G.711 无需描述子（行业默认）；AAC 在 PS 里缺少
 *     MPEG-4 es_info 描述子，ffmpeg 解码会报 channel element 告警（视频不受
 *     影响）。要支持 AAC 需给 ps_muxer_add_stream 传 AudioSpecificConfig。
 *   - 一个槽位只对应一路流；同一流名的重连会另开新槽位，时间上连续。
 */
#include "zms/core/stream/hub.h"
#include "zms/ops/config/config.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 应用 [storage] 配置；enable 非 0 时注册"推流即录制"钩子。
 * 应在服务器启动阶段调用一次（此时 poller 还没起来，钩子在真正开录时才取 poller）。
 */
ZMS_API void zms_zs_recorder_configure(const zms_storage_config *cfg);

/**
 * 为某一路媒体源启动云存储录制（幂等；未启用时直接返回 OK）。
 * @param channel_key 业务主键（设备内码_通道内码），写入槽位头作为回放检索键；
 *                    为 NULL/空 时回落到 "<app>/<stream>"。
 */
ZMS_API zn_err_t zms_zs_recorder_start(zms_media_source *src, const char *channel_key);

/** 停止某一路的录制：刷盘 + 封口 + 释放。幂等。 */
ZMS_API void zms_zs_recorder_stop(zms_media_source *src);

/** 是否启用。 */
ZMS_API int zms_zs_recorder_enabled(void);

/**
 * 槽位池统计（运维/接口用）。任一出参可为 NULL。未启用时全部为 0。
 * @param used_bytes 已占用字节（含已封口与正在写）
 * @param capacity   池容量字节
 */
ZMS_API void zms_zs_recorder_stats(uint32_t *slots, uint32_t *free_slots, uint64_t *used_bytes,
                                   uint64_t *capacity);

/** 取槽位池句柄（借用，供录像查询层按 stream+时间检索）；未就绪返回 NULL。 */
struct zs_pool;
ZMS_API struct zs_pool *zms_zs_recorder_pool(void);

/** 取当前存储配置（借用只读指针；供任务层拼兜底拉流 URL）。 */
ZMS_API const zms_storage_config *zms_zs_recorder_config(void);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_RECORDER_EGRESS_ZS_RECORDER_H */
