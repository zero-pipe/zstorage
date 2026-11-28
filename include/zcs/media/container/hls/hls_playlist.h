#ifndef ZCS_MEDIA_CONTAINER_HLS_HLS_PLAYLIST_H
#define ZCS_MEDIA_CONTAINER_HLS_HLS_PLAYLIST_H

/**
 * @file hls_playlist.h
 * @brief M3U8 播放列表生成接口（基于 zm libhls hls_m3u8 API）。
 *
 * 使播放/业务层无需包含 `hls-m3u8.h`；HLS maker 经本薄封装驱动 EXT-X-MAP /
 * 分片条目 / 列表渲染。
 */
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_hls_m3u8 zcs_hls_m3u8;

/** @param live 滑动窗口/直播参数（透传 libhls）；@param version 3=TS，7=fMP4。 */
ZCS_API zcs_hls_m3u8 *zcs_hls_m3u8_create(int live, int version);
/** 释放 M3U8 生成器；对 NULL 安全。 */
ZCS_API void zcs_hls_m3u8_destroy(zcs_hls_m3u8 *m);

/** EXT-X-MAP init 分片 URI。@return 0 成功。 */
ZCS_API int zcs_hls_m3u8_set_x_map(zcs_hls_m3u8 *m, const char *name);

/** 追加媒体分片条目。@return 0 成功。 */
ZCS_API int zcs_hls_m3u8_add(zcs_hls_m3u8 *m, const char *name, int64_t pts_ms, int64_t duration_ms,
                             int discontinuity);

/** 渲染播放列表文本。@param eof 1=追加 EXT-X-ENDLIST。@return 0 成功。 */
ZCS_API int zcs_hls_m3u8_playlist(zcs_hls_m3u8 *m, int eof, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_HLS_HLS_PLAYLIST_H */
