#ifndef ZCS_PUBLISH_INGEST_COMMON_INGEST_CODEC_H
#define ZCS_PUBLISH_INGEST_COMMON_INGEST_CODEC_H

/**
 * @file ingest_codec.h
 * @brief 直播推流 codec 适配与分发辅助。
 *
 * 编解码码流工具位于 zcs/codec。本头属于直播入站边界，因其操作 zcs_live_ingest 状态。
 */
#include "zcs/core/frame.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zn/zn_errno.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册内建直播入站 codec 输入处理器。由 zcs_modules_register_all 调用。 */
void zcs_live_ingest_codec_register_all(void);

/** 按 frame->codec 分发 @ref zcs_live_ingest_input_frame。 */
zn_err_t zcs_live_ingest_codec_input_dispatch(zcs_live_ingest *in, const zcs_frame *frame);

/** RTMP demux：分类 H264 annex-B config / 可丢弃辅助 NAL。 */
void zcs_live_ingest_h264_annexb_frame_flags(const uint8_t *annexb, size_t len, int *config_frame,
                                             int *drop_able);

/** H265 AU 累加器：将待定访问单元 flush 到 GOP 队列。 */
zn_err_t zcs_live_ingest_h265_hevc_au_flush(zcs_live_ingest *in);

/** H265 AU 累加器：清空状态（destroy / reset_upstream）。 */
void zcs_live_ingest_h265_hevc_au_reset(zcs_live_ingest *in);

/** 首帧音频前确保 G711 轨元数据。 */
void zcs_live_ingest_g711_ensure(zcs_live_ingest *in, zcs_codec_id codec);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PUBLISH_INGEST_COMMON_INGEST_CODEC_H */
