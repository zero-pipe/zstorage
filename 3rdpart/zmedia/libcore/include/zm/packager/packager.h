#ifndef ZM_PACKAGER_PACKAGER_H
#define ZM_PACKAGER_PACKAGER_H

/**
 * @file packager.h
 * @brief HLS/DASH 分段与清单接口（packager 是独立一层，不是 writer）。
 *
 * 分段策略只在这里；直播/点播的差异由调用方驱动，而不是两套 writer。
 */
#include "zm/core/packet.h"
#include "zm/core/track.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zm_packager zm_packager;

typedef struct zm_packager_ops {
    int (*write)(zm_packager *p, const zm_packet *pkt);
    int (*flush)(zm_packager *p, int force);
    void (*close)(zm_packager *p);
} zm_packager_ops;

struct zm_packager {
    const zm_packager_ops *ops;
    void *impl;
};

typedef struct zm_packager_cb {
    int (*on_init)(void *ctx, const char *name, const void *data, size_t len);
    int (*on_segment)(void *ctx, const char *name, const void *data, size_t len, int64_t pts_ms,
                      int64_t duration_ms, int discontinuity);
    int (*on_playlist)(void *ctx, const char *name, const char *text, size_t len);
} zm_packager_cb;

typedef struct zm_packager_cfg {
    int64_t segment_ms;   /**< 0 = 按视频关键帧切 */
    int live;             /**< 1 = live 窗口，0 = VOD */
    int window_segments;  /**< 保留窗口（0 = 不限） */
    const char *prefix;   /**< DASH 分片名前缀（通常为 stream 名）；NULL 用默认值 */
} zm_packager_cfg;

/** 创建 HLS(TS) packager。tracks 为已归一化的轨道描述。 */
ZM_API zm_packager *zm_hls_packager_open(const zm_track *tracks, size_t track_count,
                                            const zm_packager_cfg *cfg,
                                            const zm_packager_cb *cb, void *ctx);

/** 创建 DASH(fMP4) packager。 */
ZM_API zm_packager *zm_dash_packager_open(const zm_track *tracks, size_t track_count,
                                             const zm_packager_cfg *cfg,
                                             const zm_packager_cb *cb, void *ctx);

ZM_API int zm_packager_write(zm_packager *p, const zm_packet *pkt);

/** force != 0 时切出最后一个 segment 并生成 ENDLIST（VOD）。 */
ZM_API int zm_packager_flush(zm_packager *p, int force);

ZM_API void zm_packager_close(zm_packager *p);

#ifdef __cplusplus
}
#endif

#endif /* ZM_PACKAGER_PACKAGER_H */
