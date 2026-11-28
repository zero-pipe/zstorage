#ifndef ZMS_SRC_PROTOCOL_HTTP_SESSION_INTERNAL_H
#define ZMS_SRC_PROTOCOL_HTTP_SESSION_INTERNAL_H

/**
 * @file session_internal.h
 * @brief HTTP session 结构体与路由辅助。
 *
 * Play muxer 在此为不完整类型（LoD）：具体头文件仅由创建/销毁/驱动它们的 .c 包含。
 */
#include "zms/session/http/http_server.h"
#include "zms/protocol/http/request_reader.h"
#include "zms/core/auth.h"
#include "zms/protocol/http/session_api.h"
#include "zms/core/stream/hub.h"
#include "zms/egress/source/egress_source.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include "zn/util/buf.h"
#include <stdint.h>

/*
 * HTTP 响应头缓冲上限（字节）。
 * 组成：状态行 + Connection + CORS(4 条) + Cache-Control + Content-Type +
 * Content-Length + 可选 Content-Range/Content-Disposition。
 * 实测最长约 420 字节（HLS m3u8 + 全部 CORS 头），取 1024（约 2.4 倍余量）。
 * 超限由 http_build_header() 显式报错，绝不截断后发送。
 */
#define ZMS_HTTP_RESP_HDR_MAX 1024

/*
 * 每连接发送缓冲初始大小。
 * 原来一建连就 malloc 4MB：1000 路观众 = 4GB 常驻。初始取 128KB（一次 flush
 * 的 FLV/TS 批量远小于此），少数需要更大响应体（点播 m3u8 等）的路径用
 * zms_http_session_send_reserve() 按需增长。
 */
#define ZMS_HTTP_SEND_BUF_INIT (128u * 1024u)
/** 按需增长的上限（响应体上限）。 */
#define ZMS_HTTP_SEND_BUF_MAX (4u * 1024u * 1024u)
#include <stdio.h>

typedef enum zms_http_session_state {
    ZMS_HTTP_SESSION_STATE_IDLE,
    ZMS_HTTP_SESSION_STATE_STREAMING,
    ZMS_HTTP_SESSION_STATE_WS_STREAMING,
    ZMS_HTTP_SESSION_STATE_FILE_SENDING,
    ZMS_HTTP_SESSION_STATE_HLS_SENDING,
} zms_http_session_state;

typedef struct zms_http_session zms_http_session;

typedef struct zms_flv_live_muxer zms_flv_live_muxer;
typedef struct zms_vod_play_lane zms_vod_play_lane;

struct zms_http_session {
    zn_tcp_session *tcp;
    /** 会话所属 poller（拆除阶段不再有 tcp 时仍可用于投递 IO 池任务）。 */
    zn_poller *poller;
    zms_http_server *server;
    zms_http_request_reader *splitter;
    zms_egress_source play;
    zms_media_source *source;
    zms_vod_play_lane *vod_lane;
    zms_http_session_state state;
    int reader_attached;
    FILE *file_fp;
    uint64_t file_remain; /* 尚未读入的字节数 */
    /** 文件发送的异步预读缓冲：poller 只做发送，fread 在 IO 池。 */
    uint8_t *file_rd_buf;
    size_t file_rd_len;
    size_t file_rd_off;
    int file_reading;
    int file_eof;
    size_t hls_send_len;
    size_t hls_send_off;
    const char *play_event;
    /** 当前请求 URL 的查询串（鉴权 token 等），每次请求覆盖。 */
    char params[ZMS_AUTH_PARAMS_MAX];
    /**
     * 异步任务引用计数与关闭标记。
     * 关闭时不再立即 free：等最后一个异步任务释放后再销毁，避免
     * 在途任务解引用已释放的会话（历史上靠 zn_tcp_session_user 猜测存活）。
     */
    int async_refs;
    int closed;
    uint64_t
        play_start_ms; /**< 播放开始 zn_monotonic_ms()；供 on_play_stop 上报 duration_ms */
    int ws_mode;
    uint8_t *send_buf;
    size_t send_cap;
};

struct zms_http_server {
    zn_tcp_server *tcp;
    zn_tcp_server *tcp_tls;
    char api_secret[128];
    zn_poller *poller;
    zn_poller_pool *poller_pool;
    const struct zms_config *cfg;
#if defined(ZMS_ENABLE_WEBRTC) && ZMS_ENABLE_WEBRTC
    struct zms_webrtc_server *webrtc;
#endif
};

extern struct zms_http_server *g_http_service_instance;

void zms_http_session_stop_stream(zms_http_session *hs);
void zms_http_session_flush(zms_http_session *hs);

void zms_http_response_send_error(zms_http_session *hs, int code, const char *reason);
void zms_http_response_send_json(zms_http_session *hs, int status, const char *body,
                                 size_t body_len);
void zms_http_response_send_bytes(zms_http_session *hs, int status, const char *ctype,
                                  const void *body, size_t body_len);
void zms_http_response_send_bytes_buf(zms_http_session *hs, int status, const char *ctype,
                                      zn_buf *body);

void zms_http_route_parse_flv_path(const char *path, char *app, char *stream, uint64_t *seek_ms);
/** 直播 HTTP-TS {app}/{stream}.ts（排除 HLS 分片 {stream}.{N}.ts）*/
void zms_http_request_vhost(const zms_http_request *req, char *out, size_t cap);
int zms_http_route_parse_live_ts_path(const char *path, char *app, char *stream);
int zms_http_route_parse_mp4_path(const char *path, char *app, char *stream);
int zms_http_route_parse_dash_path(const char *path, char *app, char *stream, char *file,
                                   size_t file_cap);
int zms_http_route_parse_vod_hls_path(const char *path, char *app, char *stream, char *file,
                                      size_t file_cap);
int zms_http_route_parse_hls_path(const char *path, char *app, char *stream, char *file,
                                  size_t file_cap);

/** 直播 m3u8 0.ts 改为 /{app}/{stream}.0.ts 绝对路径 */
size_t zms_http_route_rewrite_live_hls_m3u8(char *buf, size_t cap, const char *app,
                                            const char *stream);

/** 点播 m3u8：分片行改为 /{app}/{dir}/绝对路径（与磁盘 URL 一致） */
size_t zms_http_route_rewrite_vod_hls_m3u8(char *buf, size_t cap, const char *app,
                                           const char *m3u8_rel);

void zms_http_live_flv_start(zms_http_session *hs, zms_media_source *src, const char *app,
                             const char *stream, const char *ws_key);
void zms_http_live_ts_start(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream);
void zms_http_vod_flv_start(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, uint64_t seek_ms, const char *range_hdr);
void zms_http_vod_mp4_handle(zms_http_session *hs, zms_media_source *src, const char *app,
                            const char *stream, const char *range_hdr, int head_only);

void zms_http_live_hls_handle(zms_http_session *hs, zms_media_source *src, const char *app,
                             const char *stream, const char *file);
void zms_http_live_dash_handle(zms_http_session *hs, zms_media_source *src, const char *app,
                              const char *stream, const char *file);
void zms_http_vod_hls_handle(zms_http_session *hs, const char *vhost, zms_media_source *src,
                            const char *app, const char *stream, const char *file);

void zms_http_response_send_static_file(zms_http_session *hs, zms_media_source *src,
                                        const char *path, const char *ctype, const char *player);
void zms_http_response_send_vod_flv_file(zms_http_session *hs, zms_media_source *src,
                                         const char *flv_path, const char *range_hdr,
                                         uint64_t query_seek_ms);
void zms_http_response_send_vod_mp4_file(zms_http_session *hs, zms_media_source *src,
                                         const char *mp4_path, const char *range_hdr,
                                         int head_only);
void zms_http_response_send_hls_body(zms_http_session *hs, const char *ctype, size_t body_len);

/** 按绝对路径下载附件（Content-Disposition: attachment）。 */
void zms_http_response_send_download_file(zms_http_session *hs, const char *path);

/**
 * 播放鉴权（HTTP 拉流）：上游签发的 token 在请求参数里，本地同步验签。
 * @return 1 允许；0 拒绝（调用方回 403）
 */
int zms_http_session_authorize_play(zms_http_session *hs, const char *player,
                                    zms_media_source *src);

/* 异步任务生命周期与只读访问器见 zms/protocol/http/session_api.h（对外边界）。 */

/** 把关闭文件投递到 IO 池（poller 上禁止 fclose）；fp 所有权转移。 */
void zms_http_close_file_async(FILE *fp, zn_poller *pol);

/**
 * 确保发送缓冲至少 need 字节（按需 realloc，原有内容保留）。
 * @return 0 成功；-1 失败或超过 ZMS_HTTP_SEND_BUF_MAX
 */
int zms_http_session_send_reserve(zms_http_session *hs, size_t need);

#endif /* ZMS_SRC_PROTOCOL_HTTP_SESSION_INTERNAL_H */
