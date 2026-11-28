/**
 * @file live_muxer.h
 * @brief 直播 HTTP-FLV 复用器。
 */
#ifndef ZCS_PLAY_HTTP_FLV_LIVE_MUXER_H
#define ZCS_PLAY_HTTP_FLV_LIVE_MUXER_H

struct zcs_media_source;
#include "zcs/play/sink.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_flv_live_muxer zcs_flv_live_muxer;

/** HTTP-FLV 直播：play 经 protocol_dispatch，媒体帧经 egress_pipeline */
ZCS_API zcs_flv_live_muxer *zcs_flv_live_muxer_create(struct zcs_media_source *src);
/** 重新绑定媒体源（借用，不转移所有权）；用于源信息晚到的场景。 */
ZCS_API void zcs_flv_live_muxer_bind_source(zcs_flv_live_muxer *m, struct zcs_media_source *src);
struct zn_poller;
/** 绑定 I/O poller：使 tag 工作区走 poller 本地池，并传给内部 egress pipeline。 */
ZCS_API void zcs_flv_live_muxer_bind_poller(zcs_flv_live_muxer *m, struct zn_poller *pol);
/** 销毁复用器：detach reader、清待发、归还缓冲、销毁 egress pipeline。对 NULL 安全。 */
ZCS_API void zcs_flv_live_muxer_destroy(zcs_flv_live_muxer *m);
/**
 * 写 FLV header 并重置时间轴、确定音视频轨道与音频帧步长。
 * @param has_audio/has_video 声明的轨道（与源实际轨道取并）
 * @return ZN_OK；参数非法 ZN_ERR_INVALID
 */
ZCS_API zn_err_t zcs_flv_live_muxer_start(zcs_flv_live_muxer *m, int has_audio, int has_video,
                                           uint8_t *out, size_t cap, size_t *out_len);
/**
 * 产出一个 FLV tag（onMetaData / config / 媒体帧按顺序）。
 * @return 1 产出成功；0 暂无数据；-1 错误；ZCS_SINK_NEED_BIGGER 缓冲不足（out_len 给出所需字节，
 *         换更大缓冲重试即可，已取出的帧会暂存不丢）
 */
ZCS_API int zcs_flv_live_muxer_next(zcs_flv_live_muxer *m, uint8_t *out, size_t cap,
                                    size_t *out_len);

/* zcs_flv_live_muxer_next 遵循 zcs/play/sink.h 的统一取数契约。 */

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_HTTP_FLV_LIVE_MUXER_H */
