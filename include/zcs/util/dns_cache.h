/**
 * @file dns_cache.h
 * @brief 主机名解析缓存：把 poller 上的重复 DNS 降到“每主机一次”。
 *
 * 背景（见 docs/stream-pipeline.md）：poller 线程不允许做阻塞
 * DNS。各拉流客户端连接时传主机名会让 `zn_socket_connect` 在 poller 上做
 * `getaddrinfo`。这里给一个进程内缓存：同一主机只解析一次，之后是纯内存查找。
 *
 * 限制（明确写清楚，避免误解）：
 *   - 首次解析仍是同步的，发生在调用线程上。为了把这一次也挪走，启动阶段用
 *     zcs_dns_cache_warm() 预热配置里的固定地址；运行期新增的任意主机，
 *     首次解析会同步发生一次（可接受，且只发生一次）。
 *   - 不实现 TTL/失效。长跑场景下 DNS 变更需重启或调用 zcs_dns_cache_flush()。
 */
#ifndef ZCS_UTIL_DNS_CACHE_H
#define ZCS_UTIL_DNS_CACHE_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 解析主机名为 IP 文本（IPv4 优先；无 IPv4 时用 IPv6）。
 * 主机本身就是 IP 文本时直接回填，不查表也不解析。
 * @return 0 成功；-1 失败（out 置空串）
 */
ZCS_API int zcs_dns_resolve(const char *host, char *out_ip, size_t cap);

/**
 * 连接目标：能解析就返回 IP（写入 buf），否则原样返回 host。
 * 供各客户端在 connect 前调用，失败时退回主机名让底层报出真实错误。
 */
ZCS_API const char *zcs_dns_target(const char *host, char *buf, size_t cap);

/** 预热一个 URL（http://、rtsp://、rtmp:// 等均可）或裸主机名；失败静默。 */
ZCS_API void zcs_dns_cache_warm(const char *url);

/** 清空缓存（测试或运维用）。 */
ZCS_API void zcs_dns_cache_flush(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_UTIL_DNS_CACHE_H */
