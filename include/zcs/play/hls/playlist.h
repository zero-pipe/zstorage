/**
 * @file playlist.h
 * @brief HLS m3u8 清单生成。
 */
#ifndef ZCS_PLAY_HLS_PLAYLIST_H
#define ZCS_PLAY_HLS_PLAYLIST_H

/**
 * HLS 切片索引与 m3u8（m3u8 由 vendored libhls/hls-m3u8 生成；TS 字节存于内存环）
 */
#include "zcs/zcs_export.h"
#include "zn/util/buf.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_http_hls_segment {
    char name[64];
    uint32_t dur_ms;
    zn_buf *buf;
} zcs_http_hls_segment;

typedef struct zcs_http_hls_playlist zcs_http_hls_playlist;

typedef struct zcs_http_hls_playlist_opts {
    float segment_duration_sec;
    unsigned segment_count;
    /** 1 = HLS version 7 fMP4 (.m4s + init.mp4)，用于 VP8/VP9/AV1 等 TS 不友好编码 */
    int use_fmp4;
} zcs_http_hls_playlist_opts;

/**
 * 创建切片索引/清单对象。
 * @param opts 段时长（默认 2s）、保留段数（默认 3）、是否 fMP4（version 7）
 * @return 新对象；内存不足返回 NULL
 */
ZCS_API zcs_http_hls_playlist *zcs_http_hls_playlist_create(const zcs_http_hls_playlist_opts *opts);
/** 释放切片环、init 段与 m3u8 生成器；对 NULL 安全。 */
ZCS_API void zcs_http_hls_playlist_destroy(zcs_http_hls_playlist *m);
/** 清空切片环与 m3u8（点播新会话） */
ZCS_API void zcs_http_hls_playlist_reset(zcs_http_hls_playlist *m);

/** 写入完整 TS 切片并更新 m3u8 列表（由 hls_media 回调触发） */
ZCS_API zn_err_t zcs_http_hls_playlist_push_segment(zcs_http_hls_playlist *m, const void *data,
                                                     size_t len, int64_t pts_ms, int64_t dts_ms,
                                                     uint64_t duration_ms, int discontinuity);

/**
 * @brief 写入 TS 切片并接管 @a buf 所有权（调用方不得再 unref）。
 */
ZCS_API zn_err_t zcs_http_hls_playlist_push_segment_buf(zcs_http_hls_playlist *m, zn_buf *buf,
                                                         int64_t pts_ms, int64_t dts_ms,
                                                         uint64_t duration_ms, int discontinuity);

/**
 * 生成当前滑动窗口的 m3u8 文本（直播）。
 * @param out 输出缓冲，至少 32 字节；容量不足返回 ZN_ERR_BUFFER_TOO_SMALL
 * @param out_len 实际写入长度（不含 NUL）
 */
ZCS_API zn_err_t zcs_http_hls_playlist_build_m3u8(const zcs_http_hls_playlist *m, char *out,
                                                   size_t cap, size_t *out_len);

/** 点播：按总时长生成完整 m3u8 索引（VLC 显示总时长）；add_endlist 在整片结束时可置 1 */
ZCS_API zn_err_t zcs_http_hls_playlist_build_vod_index_m3u8(const zcs_http_hls_playlist *m,
                                                             char *out, size_t cap, size_t *out_len,
                                                             uint64_t total_dur_ms,
                                                             uint32_t seg_dur_ms, int add_endlist);

/** 查询指定名字的分片是否仍存在于滑动窗口（TS 除非 fMP4，要求 >=188 字节）。 */
ZCS_API int zcs_http_hls_playlist_has_segment(const zcs_http_hls_playlist *m, const char *name);
/**
 * 拷贝指定分片字节到调用方缓冲。
 * @return ZN_OK 命中；ZN_ERR_BUFFER_TOO_SMALL 容量不足；ZN_ERR_INVALID 未命中/参数非法
 */
ZCS_API zn_err_t zcs_http_hls_playlist_copy_segment(zcs_http_hls_playlist *m, const char *name,
                                                     uint8_t *buf, size_t cap, size_t *out_len);
/** 命中切片则 zn_buf_ref 给调用方；调用方须 zn_buf_unref */
ZCS_API zn_err_t zcs_http_hls_playlist_ref_segment(zcs_http_hls_playlist *m, const char *name,
                                                    zn_buf **out_buf, size_t *out_len);

/** 已就绪切片数量；out_name/out_len 可选，返回最新一片名与字节数 */
ZCS_API int zcs_http_hls_playlist_segment_count(const zcs_http_hls_playlist *m);
/**
 * 取最新就绪分片（低延迟起播用）。
 * @param out_name/name_cap 可选，返回分片名；out_len 可选，返回字节数
 * @return 1 有可用分片；0 无分片或最新段尚未就绪
 */
ZCS_API int zcs_http_hls_playlist_latest_segment(const zcs_http_hls_playlist *m, char *out_name,
                                                 size_t name_cap, size_t *out_len);

/** 查询清单是否使用 fMP4（.m4s + init.mp4）模式。 */
ZCS_API int zcs_http_hls_playlist_use_fmp4(const zcs_http_hls_playlist *m);

/** fMP4 init segment（init.mp4）；@a buf 所有权转移给 maker */
ZCS_API zn_err_t zcs_http_hls_playlist_store_init_segment(zcs_http_hls_playlist *m, zn_buf *buf);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_HLS_PLAYLIST_H */
