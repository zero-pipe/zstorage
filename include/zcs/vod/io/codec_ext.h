/**
 * @file codec_ext.h
 * @brief VOD 容器扩展 codec。
 */
#ifndef ZCS_VOD_IO_CODEC_EXT_H
#define ZCS_VOD_IO_CODEC_EXT_H

/**
 * 点播编解码扩展模板。
 */
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_VOD_CODEC_HEVC "hevc"

typedef struct zcs_vod_codec_ops {
    const char *name;
    int (*probe_mp4)(const char *file_path);
    zn_err_t (*lane_prepare)(void *ctx);
    void (*lane_destroy)(void *ctx);
} zcs_vod_codec_ops;

/**
 * 注册一个点播扩展 codec（按 name 去重，重名覆盖旧项；最多 8 个）。
 * ops 指针被借用而非拷贝，调用方须保证其生命周期贯穿进程。
 */
ZCS_API void zcs_vod_codec_ext_register(const zcs_vod_codec_ops *ops);
/** 注册内置扩展 codec（HEVC 等）；幂等，重复调用无副作用。 */
ZCS_API void zcs_vod_codec_ext_register_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_IO_CODEC_EXT_H */
