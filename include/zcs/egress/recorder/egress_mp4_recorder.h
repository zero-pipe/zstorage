#ifndef ZCS_EGRESS_RECORDER_EGRESS_MP4_RECORDER_H
#define ZCS_EGRESS_RECORDER_EGRESS_MP4_RECORDER_H

/**
 * @file egress_mp4_recorder.h
 * @brief 直播 MP4 云录像（对齐 ZLM enable_mp4 / on_record_mp4）。
 *
 * 目录规则（唯一）：
 *   {record.root}/{stream}/{YYYY-MM-dd}/{YYYYMMDDHHMMSS}[_n].mp4
 * - 不含 app：record 已表示录制；app 只进 hook 元数据
 * - stream：一路流；日期目录：当天切片；文件名：起录时分秒（同秒冲突加 _n）
 */
#include "zcs/core/stream/hub.h"
#include "zcs/ops/config/config.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"

struct zn_poller;
typedef struct zn_poller zn_poller;

#ifdef __cplusplus
extern "C" {
#endif

/** 从 [record] 应用 root / mp4_max_second（服务启动时调用一次）。 */
ZCS_API void zcs_mp4_recorder_configure(const zcs_record_config *rec);

/**
 * 若 src->enable_mp4 且尚未启动，则创建录制器并绑定 poller 定时 tick。
 * @return ZN_OK 或已在录制；失败返回错误码。
 */
ZCS_API zn_err_t zcs_mp4_recorder_start(zcs_media_source *src, zn_poller *poller);

/** 停止录制：落盘当前文件、触发 on_record_mp4、释放资源。幂等。 */
ZCS_API void zcs_mp4_recorder_stop(zcs_media_source *src);

/** 校验绝对路径是否落在录像 root 下（禁止 ..）。1=安全。 */
ZCS_API int zcs_mp4_recorder_path_under_root(const char *file_path);

/** 当前录像 root（configure 后有效）。 */
ZCS_API const char *zcs_mp4_recorder_root(void);

/**
 * 按 [record] max_total_mb 清理最旧录像（阻塞，应在 IO 线程调用）。
 * 返回删除的文件数。
 */
ZCS_API int zcs_mp4_recorder_enforce_retention(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_RECORDER_EGRESS_MP4_RECORDER_H */
