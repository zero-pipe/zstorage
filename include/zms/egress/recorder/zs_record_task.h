/**
 * @file zs_record_task.h
 * @brief 云存储录制任务：按"通道"主动拉流并录制，而非见流就录。
 *
 * zstorage 的录制不是旁路钩子，而是一个**主动拉流**的中心存储服务：
 *   平台下发"录某路/某通道" -> 给出该路在 ZMS 的取流地址（RTSP/RTMP/FLV）
 *   -> 本模块用拉流代理把流拉到本进程的 media_source
 *   -> 拉流就绪（视频参数集齐全、已 publish）后，为该源挂上 PS 槽位录制器。
 *
 * 任务以 **channel_key**（形如 "<设备内码>_<通道内码>"）为主键去重和检索；
 * 调用方只给 channel_key 不给 URL 时，按 "rtsp://<host>:<port>/live/<channel_key>"
 * 兜底拼装（host/port 由 [storage] pull_host/pull_port 配置）。
 *
 * 生命周期：
 *   start(channel_key, url) -> 建拉流代理 -> 就绪后开始录 -> 断流自动封口；
 *   stop(channel_key)       -> 停代理、封口、释放；
 *   任务可重复 start（幂等），URL 变化会重建代理。
 */
#ifndef ZMS_EGRESS_RECORDER_ZS_RECORD_TASK_H
#define ZMS_EGRESS_RECORDER_ZS_RECORD_TASK_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** channel_key 最大长度（与 stream 名同量级）。 */
#define ZS_CHANNEL_KEY_MAX 128

/** 一个录制任务的对外视图。 */
typedef struct zs_record_task_info {
    char channel_key[ZS_CHANNEL_KEY_MAX];
    char pull_url[768];
    int recording;   /**< 1=正在录（媒体源已就绪并挂上录制器） */
    int online;      /**< 1=拉流已连上（publish 过） */
    uint64_t started_utc_ms; /**< 任务创建时间 */
} zs_record_task_info;

/**
 * 启动一个录制任务（幂等）。
 * @param channel_key  业务主键（必填，如 "1001_0001"）
 * @param pull_url     取流地址；NULL/空 时按配置兜底拼装 rtsp://<host>:<port>/live/<channel_key>
 * @return ZN_OK 已启动（或已在录）；ZN_ERR_INVALID 参数非法；ZN_ERR_STATE 存储未启用/池不可用
 */
ZMS_API zn_err_t zms_zs_record_start(const char *channel_key, const char *pull_url);

/** 停止并移除一个录制任务（幂等）：停代理、封口、释放录制器。 */
ZMS_API void zms_zs_record_stop(const char *channel_key);

/** 取任务信息；找到返回 1，否则 0。 */
ZMS_API int zms_zs_record_get(const char *channel_key, zs_record_task_info *out);

/** 任务总数。 */
ZMS_API uint32_t zs_record_task_count(void);

/** 按索引取任务信息（遍历用）；越界返回 0。 */
ZMS_API int zs_record_task_at(uint32_t idx, zs_record_task_info *out);

/* ── 录像查询（按通道 + 时间）─────────────────────────────────────────────
 * 返回该通道在 [from_ms, to_ms] 内有重叠的已封口槽位段。channel_key 为 NULL/空
 * 表示查所有通道。结果按开始时间升序写入 out（最多 max 条）。
 */
typedef struct zs_record_segment {
    uint32_t slot_id;
    char channel_key[ZS_CHANNEL_KEY_MAX];
    uint64_t start_ms; /**< 段起始绝对时间（UTC 毫秒） */
    uint64_t end_ms;   /**< 段结束绝对时间 */
    uint64_t bytes;    /**< 段数据字节数（PS 数据区实际写入量） */
} zs_record_segment;

/** @return 写入 out 的条数（可能被 max 截断） */
ZMS_API uint32_t zms_zs_record_query(const char *channel_key, uint64_t from_ms, uint64_t to_ms,
                                     zs_record_segment *out, uint32_t max);

/**
 * 按槽位号删除录像（幂等：已 FREE/不存在算成功）。仅删除 SEALED 且未被回放钉住的槽位。
 * @param slots 槽位号数组
 * @return 删除的槽位数
 */
ZMS_API uint32_t zms_zs_record_delete(const uint32_t *slots, uint32_t n_slots);

/** 存储节点容量统计（容量/已用/槽位/空闲），供 node/status 使用。 */
ZMS_API void zms_zs_record_node_status(uint32_t *slots, uint32_t *free_slots, uint64_t *used_bytes,
                                       uint64_t *cap_bytes);

/** 取某槽位的绝对文件路径（回放定位用）。@return 0 成功；-1 失败 */
ZMS_API int zms_zs_record_slot_path(uint32_t slot, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_RECORDER_ZS_RECORD_TASK_H */
