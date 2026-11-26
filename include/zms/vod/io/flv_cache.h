/**
 * @file flv_cache.h
 * @brief VOD FLV 缓存文件。
 */
#ifndef ZMS_VOD_IO_FLV_CACHE_H
#define ZMS_VOD_IO_FLV_CACHE_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include "zn/poller/poller.h"
#include <stddef.h>

struct zms_media_source;

#ifdef __cplusplus
extern "C" {
#endif

/** 缓存路径：{root}/{app}/{stream}.mp4.flv */
ZMS_API int zms_vod_flv_cache_path(const char *app, const char *stream, char *out, size_t out_cap);

/** 缓存存在且不比 mp4 旧 */
ZMS_API int zms_vod_flv_cache_valid(const char *app, const char *stream);

/** 同步生成 FLV 缓存（首次 HTTP 点播可能耗时） */
ZMS_API zn_err_t zms_vod_flv_cache_ensure(const char *app, const char *stream,
                                           struct zms_media_source *src, zn_poller *pol);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_IO_FLV_CACHE_H */
