/**
 * @file playlist.h
 * @brief DASH MPD 清单生成。
 */
#ifndef ZCS_PLAY_DASH_PLAYLIST_H
#define ZCS_PLAY_DASH_PLAYLIST_H

#include "zcs/zcs_export.h"
#include "zn/util/buf.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

struct dash_mpd_t;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_http_dash_segment {
    char name[128];
    zn_buf *buf;
} zcs_http_dash_segment;

typedef struct zcs_http_dash_playlist zcs_http_dash_playlist;

typedef struct zcs_http_dash_playlist_opts {
    float segment_duration_sec;
    unsigned segment_count;
    const char *prefix;
} zcs_http_dash_playlist_opts;

/**
 * 创建 DASH MPD 播放列表及分片环形缓存。
 * 内存环容量取 max(segment_count,6)*2（2×MPD 窗口，降低分片被覆盖后 404）；
 * prefix 为空默认 "live"。内部含互斥量，可多线程访问。
 * @return 成功返回对象；分配失败返回 NULL（用 destroy 释放）。
 */
ZCS_API zcs_http_dash_playlist *
zcs_http_dash_playlist_create(const zcs_http_dash_playlist_opts *opts);
/** 销毁播放列表，释放 MPD、分片缓冲与互斥量；m 可为 NULL。 */
ZCS_API void zcs_http_dash_playlist_destroy(zcs_http_dash_playlist *m);

/** 取内部 dash_mpd 句柄（借用，供打包器直接喂分片）；m 为 NULL 返回 NULL。 */
ZCS_API struct dash_mpd_t *zcs_http_dash_playlist_mpd(zcs_http_dash_playlist *m);

/** dash_mpd segment callback：缓存 init/.m4s 分片 */
ZCS_API int zcs_http_dash_playlist_on_segment(void *param, int track, const void *data,
                                              size_t bytes, int64_t pts, int64_t dts,
                                              int64_t duration, const char *name);

/** 刷新并拷出 MPD 文本到 out（持锁）；容量不足或空返回错误。 */
ZCS_API zn_err_t zcs_http_dash_playlist_build_mpd(zcs_http_dash_playlist *m, char *out, size_t cap,
                                                   size_t *out_len);
/** 拷贝 on_segment 后缓存的 MPD（无 recorder 锁，供 HTTP 热路径） */
ZCS_API zn_err_t zcs_http_dash_playlist_copy_mpd(const zcs_http_dash_playlist *m, char *out,
                                                  size_t cap, size_t *out_len);
/** 按请求名（可做时间戳容错解析）拷出分片内容到 buf；未命中/超容量返回错误。 */
ZCS_API zn_err_t zcs_http_dash_playlist_copy_segment(zcs_http_dash_playlist *m, const char *name,
                                                      uint8_t *buf, size_t cap, size_t *out_len);
/**
 * 按请求名取分片 buf 的引用（增加引用计数，避免零拷贝发送时被环覆盖）。
 * @param out_buf 输出借用的 zn_buf；成功时调用方负责 unref
 * @return ZN_OK 成功；未命中或超容量返回错误，*out_buf 置 NULL。
 */
ZCS_API zn_err_t zcs_http_dash_playlist_ref_segment(zcs_http_dash_playlist *m, const char *name,
                                                     zn_buf **out_buf, size_t *out_len);
/** 缓存中是否已有非 init 的媒体分片（.m4v/.m4a） */
ZCS_API int zcs_http_dash_playlist_has_media_segment(const zcs_http_dash_playlist *m);
/** 当前缓存中视频+音频分片数（不含 init）。 */
ZCS_API int zcs_http_dash_playlist_segment_count(const zcs_http_dash_playlist *m);
/** MPD 分片名前缀（借用）；空则回退 "live"。 */
ZCS_API const char *zcs_http_dash_playlist_prefix(const zcs_http_dash_playlist *m);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_DASH_PLAYLIST_H */
