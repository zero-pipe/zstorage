#ifndef ZCS_EGRESS_SOURCE_EGRESS_SOURCE_H
#define ZCS_EGRESS_SOURCE_EGRESS_SOURCE_H

/**
 * @file egress_source.h
 * @brief 直播 GOP 缓存与点播 play-buffer 读者的统一读路径。
 *
 * 直播附着 zcs_gop_reader；VOD 附着 zcs_vod_buffer_reader。
 * 上层（egress_pipeline、RTP play）经本句柄读取 mux 槽位。
 */
#include "zcs/core/gop/queue.h"
#include "zcs/core/stream/hub.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_egress_source {
    zcs_media_source *source;
    zcs_media_subscriber readers;
    int is_live;
} zcs_egress_source;

/** 初始化句柄为零值（不含任何读者）；在使用前调用。 */
ZCS_API void zcs_egress_source_init(zcs_egress_source *p);
/** 解除订阅并清零句柄（幂等，可对未打开的句柄调用）。 */
ZCS_API void zcs_egress_source_close(zcs_egress_source *p);

/** 在当前 GOP 关键帧处打开直播读者。 */
ZCS_API zn_err_t zcs_egress_source_open_live_gop(zcs_media_source *src, zcs_egress_source *p);

/** 在最新 GOP sync 处打开直播读者（RTSP attach use_gop）。 */
ZCS_API zn_err_t zcs_egress_source_open_live_key(zcs_media_source *src, zcs_egress_source *p);

/**
 * @param seek_live 1 = 追直播边缘；0 = 从 ring 头开始
 */
ZCS_API zn_err_t zcs_egress_source_open_live(zcs_media_source *src, int seek_live,
                                              zcs_egress_source *p);

/** 打开 VOD 读者；seek 到首个视频 sync 关键帧。 */
ZCS_API zn_err_t zcs_egress_source_open_vod(zcs_media_source *src, zcs_egress_source *p);

/**
 * 读取一条 mux 槽位。
 * @return 1 成功，0 无数据，-1 错误
 */
ZCS_API int zcs_egress_source_read_muxed(zcs_egress_source *p, zcs_gop_slot *slot, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_EGRESS_SOURCE_EGRESS_SOURCE_H */
