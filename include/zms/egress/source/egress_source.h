#ifndef ZMS_EGRESS_SOURCE_EGRESS_SOURCE_H
#define ZMS_EGRESS_SOURCE_EGRESS_SOURCE_H

/**
 * @file egress_source.h
 * @brief 直播 GOP 缓存与点播 play-buffer 读者的统一读路径。
 *
 * 直播附着 zms_gop_reader；VOD 附着 zms_vod_buffer_reader。
 * 上层（egress_pipeline、RTP play）经本句柄读取 mux 槽位。
 */
#include "zms/core/gop/queue.h"
#include "zms/core/stream/hub.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_egress_source {
    zms_media_source *source;
    zms_media_subscriber readers;
    int is_live;
} zms_egress_source;

/** 初始化句柄为零值（不含任何读者）；在使用前调用。 */
ZMS_API void zms_egress_source_init(zms_egress_source *p);
/** 解除订阅并清零句柄（幂等，可对未打开的句柄调用）。 */
ZMS_API void zms_egress_source_close(zms_egress_source *p);

/** 在当前 GOP 关键帧处打开直播读者。 */
ZMS_API zn_err_t zms_egress_source_open_live_gop(zms_media_source *src, zms_egress_source *p);

/** 在最新 GOP sync 处打开直播读者（RTSP attach use_gop）。 */
ZMS_API zn_err_t zms_egress_source_open_live_key(zms_media_source *src, zms_egress_source *p);

/**
 * @param seek_live 1 = 追直播边缘；0 = 从 ring 头开始
 */
ZMS_API zn_err_t zms_egress_source_open_live(zms_media_source *src, int seek_live,
                                              zms_egress_source *p);

/** 打开 VOD 读者；seek 到首个视频 sync 关键帧。 */
ZMS_API zn_err_t zms_egress_source_open_vod(zms_media_source *src, zms_egress_source *p);

/**
 * 读取一条 mux 槽位。
 * @return 1 成功，0 无数据，-1 错误
 */
ZMS_API int zms_egress_source_read_muxed(zms_egress_source *p, zms_gop_slot *slot, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_EGRESS_SOURCE_EGRESS_SOURCE_H */
