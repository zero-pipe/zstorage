/**
 * @file webapi_internal.h
 * @brief Web API 处理器的跨文件共享类型与助手（仅 ops/api/webapi 内部使用）。
 *
 * webapi 处理器按主题拆到多个 .c（media / stats / rtp / record / webrtc），
 * 共享查询参数视图、JSON 缓冲与少量助手；这里只暴露这些契约，不放业务。
 */
#ifndef ZMS_OPS_API_WEBAPI_WEBAPI_INTERNAL_H
#define ZMS_OPS_API_WEBAPI_WEBAPI_INTERNAL_H

#include "zms/ops/api/webapi/webapi.h"
#include "zms/ops/config/config.h"
#include "zms/core/auth.h"
#include "zms/core/event.h"
#include "zms/device/onvif/onvif.h"
#include "zn/util/buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 解析后的 query 参数视图（各字段为 NUL 结尾的定长缓冲）。 */
typedef struct zms_webapi_query_map {
    char schema[ZMS_SCHEMA_MAX];
    char vhost[64];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    char stream_id[ZMS_STREAM_MAX];
    char port[16];
    char secret[128];
    char force[8];
    char url[ZMS_CFG_URL_MAX];
    char key[256];
    char kind[16];
    char token[256];
    /** 接入 token 白名单（CSV，供 /index/api/auth/tokens 同步）。 */
    char tokens[1024];
    char id[64];
    char rtp_transport[16];
    char proxy_prefix[64];
    char pt[8];
    char ssrc[16];
    char tcp_mode[8];
    char dst_url[128];
    char dst_port[16];
    char file_path[ZMS_CFG_PATH_MAX];
    char stamp[32];
    char speed[16];
    /* ONVIF */
    char addr[ZMS_ONVIF_XADDR_MAX];
    char user[64];
    char pass[128];
    char profile[64];
    char x[16];
    char y[16];
    char z[16];
    char stop[8];
    char timeout[16];
    /* GB28181 */
    char channel[128];
    char device[32];
    char cmd[32];
    char call_id[96];
    char start[32];
    char end[32];
    /** 删除录像用的槽位号列表（CSV，如 "1,2,3"）。 */
    char slots[512];
    /** 单个槽位号（回放定位）。 */
    char slot[16];
    char pan[16];
    char tilt[16];
    char zoom[16];
    char tcp[8];
    char priority[8];
    char method[8];
    char description[256];
    /** 原始查询串（未解码），供鉴权验签使用。 */
    char raw_query[ZMS_AUTH_PARAMS_MAX];
} zms_webapi_query_map;

/** 顺写 JSON 缓冲（cap 为总量，len 为当前写入量）。 */
typedef struct zms_webapi_json_buf {
    char *buf;
    size_t cap;
    size_t len;
} zms_webapi_json_buf;

/** 向 json 缓冲追加格式化片段；溢出返回 -1（调用方据此回 500）。 */
int webapi_jb_append(zms_webapi_json_buf *jb, const char *fmt, ...);

/** 通用 JSON 响应：{"code":code,"msg":msg}；extra 非空时并入（须为合法 JSON 片段）。 */
size_t webapi_respond_json(int code, const char *msg, const char *extra, char *body,
                           size_t body_cap);

/** 由配置读取各协议监听端口（缺省 0）。 */
zms_media_server_ports webapi_ports_from_cfg(const zms_config *cfg);

/** 运行时长（秒），未 note_boot 时为 0。 */
uint64_t webapi_uptime_sec(void);

/** 缓冲池命中率（百分比）。 */
double webapi_buf_pool_hit_rate(const zn_buf_pool_stats *st);

/** 进程 RSS 字节数 / 打开 fd 数（Linux /proc；其它平台返回 0）。 */
uint64_t webapi_proc_rss_bytes(void);
uint64_t webapi_proc_fd_count(void);

/* ── 各主题文件对外的处理器 ─────────────────────────────────────────────── */

size_t webapi_handle_rtp_open(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                              char *body, size_t body_cap, int *http_status);
size_t webapi_handle_rtp_connect(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                                 char *body, size_t body_cap, int *http_status);
size_t webapi_handle_rtp_close(const zms_webapi_query_map *q, char *body, size_t body_cap,
                               int *http_status);
size_t webapi_handle_rtp_list(zms_webapi_json_buf *jb);

size_t webapi_handle_proxy_add(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                               char *body, size_t body_cap, int *http_status);
size_t webapi_handle_proxy_del(const zms_webapi_query_map *q, char *body, size_t body_cap,
                               int *http_status);
size_t webapi_handle_proxy_list(zms_webapi_json_buf *jb);

size_t webapi_handle_load_mp4(const zms_webapi_query_map *q, const zms_web_api_opts *opts,
                              char *body, size_t body_cap, int *http_status);
size_t webapi_handle_delete_record(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                   int *http_status);
size_t webapi_handle_seek_record(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                 int *http_status);
size_t webapi_handle_record_speed(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                  int *http_status);

size_t webapi_handle_zs_record_start(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status);
size_t webapi_handle_zs_record_stop(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                    int *http_status);
size_t webapi_handle_zs_record_list(zms_webapi_json_buf *jb);
size_t webapi_handle_zs_record_query(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                     int *http_status);
size_t webapi_handle_zs_record_delete(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status);
size_t webapi_handle_zs_node_status(char *body, size_t body_cap, int *http_status);

size_t webapi_handle_zs_playback_start(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                       int *http_status);
size_t webapi_handle_zs_playback_stop(const zms_webapi_query_map *q, char *body, size_t body_cap,
                                      int *http_status);

size_t webapi_handle_health(char *body, size_t body_cap, int *http_status);
size_t webapi_handle_statistic(char *body, size_t body_cap, int *http_status);
size_t webapi_handle_buf_pool_stats(char *body, size_t body_cap, int *http_status);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_OPS_API_WEBAPI_WEBAPI_INTERNAL_H */
