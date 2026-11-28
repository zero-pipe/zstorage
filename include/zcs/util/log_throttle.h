#ifndef ZCS_UTIL_LOG_THROTTLE_H
#define ZCS_UTIL_LOG_THROTTLE_H

/**
 * @file log_throttle.h
 * @brief 按 key 限制重复日志行（进程内）。
 */
#include "zcs/zcs_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 相同 @a key 的 warn 行之间的默认最小间隔。 */
#define ZCS_LOG_THROTTLE_WARN_MS 60000u

/**
 * 节流版 warn：相同 key 在 interval_ms 内只真正输出一次，被压制的次数在下次输出时附注。
 * @param key 节流键（最长 95 字节）；key 为空则丢弃该日志
 * @param interval_ms 最小输出间隔；0 表示不节流
 */
ZCS_API void zcs_log_warn_throttle(const char *key, uint64_t interval_ms, const char *fmt, ...);

/** 节流版 debug，语义同 warn 版本，输出级别为 debug。 */
ZCS_API void zcs_log_debug_throttle(const char *key, uint64_t interval_ms, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_UTIL_LOG_THROTTLE_H */
