#ifndef ZCS_DEMO_SERVER_RUNTIME_H
#define ZCS_DEMO_SERVER_RUNTIME_H

/**
 * 示例程序公共运行时：配置加载、日志、poller_pool、缓冲池等。
 * zcs / demo_gb28181_device 共用同一套初始化语义。
 *
 * 信号处理（仅 POSIX）：
 *   SIGTERM / SIGINT  → 设置 g_zcs_stop = 1，wait() 返回，进程优雅退出
 *   SIGHUP            → reopen 日志文件（配合 logrotate 使用）
 */
#include "zcs/ops/config/config.h"
#include "zn/poller/poller_pool.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_demo_server_args {
    const char *config_path;
    const char *log_file_override;
    /** -1 = 一直运行；>= 0 = 运行指定秒数后退出 */
    int seconds;
} zcs_demo_server_args;

typedef struct zcs_demo_server_runtime {
    zcs_config cfg;
    zn_poller_pool *poller_pool;
    zn_poller *main_poller;
    unsigned poller_count;
} zcs_demo_server_runtime;

/**
 * 全局停止标志：SIGTERM/SIGINT 信号处理器将其置 1。
 * 主循环应检查该标志以实现优雅退出。
 */
extern volatile int g_zcs_stop;

void zcs_demo_server_args_default(zcs_demo_server_args *args);

/** 解析 --config / --log / --seconds / -h；成功返回 0，help 返回 0，错误返回 1 */
int zcs_demo_server_parse_args(int argc, char **argv, zcs_demo_server_args *args);

void zcs_demo_server_print_usage(const char *prog, const char *extra);

zn_err_t zcs_demo_server_runtime_init(zcs_demo_server_runtime *rt,
                                       const zcs_demo_server_args *args);

void zcs_demo_server_runtime_fini(zcs_demo_server_runtime *rt);

/** seconds < 0 时阻塞直到收到 SIGTERM/SIGINT；否则 sleep 指定秒数后返回 */
void zcs_demo_server_runtime_wait(const zcs_demo_server_runtime *rt, int seconds);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_DEMO_SERVER_RUNTIME_H */
