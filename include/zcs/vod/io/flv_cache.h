/**
 * @file flv_cache.h
 * @brief VOD FLV 缓存文件。
 */
#ifndef ZCS_VOD_IO_FLV_CACHE_H
#define ZCS_VOD_IO_FLV_CACHE_H

#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include "zn/poller/poller.h"
#include <stddef.h>

struct zcs_media_source;

#ifdef __cplusplus
extern "C" {
#endif

/** 缓存路径：{root}/{app}/{stream}.mp4.flv */
ZCS_API int zcs_vod_flv_cache_path(const char *app, const char *stream, char *out, size_t out_cap);

/** 缓存存在且不比 mp4 旧 */
ZCS_API int zcs_vod_flv_cache_valid(const char *app, const char *stream);

/** 同步生成 FLV 缓存（首次 HTTP 点播可能耗时） */
ZCS_API zn_err_t zcs_vod_flv_cache_ensure(const char *app, const char *stream,
                                           struct zcs_media_source *src, zn_poller *pol);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_IO_FLV_CACHE_H */
