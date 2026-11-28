/**
 * @file protocol_opts.h
 * @brief 各协议入站默认选项。
 */
#ifndef ZCS_PUBLISH_INGEST_COMMON_PROTOCOL_OPTS_H
#define ZCS_PUBLISH_INGEST_COMMON_PROTOCOL_OPTS_H

#include "zcs/zcs_export.h"
#include "zcs/ops/config/config.h" /* zcs_protocol_config */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 直播推流协议选项，与 config.ini [protocol] 节字段一一对应。
 * zcs_protocol_opts 为 zcs_protocol_config 的别名，内存布局相同。
 */
typedef zcs_protocol_config zcs_protocol_opts;

/** [protocol] 默认值初始化 opts（enable_rtmp/rtsp/audio=1，其余为 0） */
ZCS_API void zcs_protocol_opts_default(zcs_protocol_opts *opts);
/** 设置进程级默认值（server 启动时用 [protocol] 配置覆盖） */
ZCS_API void zcs_protocol_opts_set_defaults(const zcs_protocol_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PUBLISH_INGEST_COMMON_PROTOCOL_OPTS_H */
