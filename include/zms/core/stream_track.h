/**
 * @file stream_track.h
 * @brief 视频/音频轨结构与辅助。
 */
#ifndef ZMS_CORE_STREAM_TRACK_H
#define ZMS_CORE_STREAM_TRACK_H

#include "zms/core/frame.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 直播流视频元数据（来自 AVC config / SPS）。 */
typedef struct zms_video_track {
    zms_codec_id codec;
    int width;
    int height;
    float fps;
    char profile_level_id[16];
    int ready;
} zms_video_track;

/** 直播流音频元数据（来自 AAC ASC / RTMP audio config）。 */
typedef struct zms_audio_track {
    zms_codec_id codec;
    int sample_rate;
    int channels;
    char asc_hex[64];
    int ready;
} zms_audio_track;

/** 清空视频轨元数据（codec/尺寸/fps/profile 归零，ready=0）；对 NULL 安全。 */
ZMS_API void zms_video_track_clear(zms_video_track *t);
/** 清空音频轨元数据（codec/采样率/声道/asc 归零，ready=0）；对 NULL 安全。 */
ZMS_API void zms_audio_track_clear(zms_audio_track *t);
/**
 * 从 RTMP AVC 序列头（extradata）解析出视频轨：设置 codec=H264、profile_level_id、宽高与 fps。
 * 先清空目标再填充；解析失败仍置 codec/ready，缺失 SPS 时宽高保持 0。
 * @param data RTMP FLV video sequence header（含 AVCDecoderConfigurationRecord）
 * @return ZN_OK；输入过短或无法抽取 SPS/PPS 返回 ZN_ERR_INVALID。
 */
ZMS_API zn_err_t zms_video_track_from_avc(zms_video_track *t, const uint8_t *data, size_t len);
/**
 * 从 RTMP 音频 tag 解析音频轨：G.711A/U 走固定 8 kHz 单声道，AAC 取 ASC 解析。
 * @return ZN_OK；非受支持编解码或数据不足返回 ZN_ERR_INVALID。
 */
ZMS_API zn_err_t zms_audio_track_from_rtmp(zms_audio_track *t, const uint8_t *data, size_t len);
/**
 * 从裸 AAC AudioSpecificConfig 解析采样率/声道并记录 asc_hex（先清空目标）。
 * ASC 解析失败时回退到 44100 Hz / 双声道，仍置 ready。
 * @return ZN_OK；asc 为空返回 ZN_ERR_INVALID。
 */
ZMS_API zn_err_t zms_audio_track_from_asc(zms_audio_track *t, const uint8_t *asc, size_t len);
/**
 * 以 G.711 参数填充音频轨。
 * @param codec 仅接受 ZMS_CODEC_G711A / ZMS_CODEC_G711U，其余返回 ZN_ERR_INVALID。
 * @param sample_rate 为 0 时默认 8000；channels 为 0 时默认 1。
 */
ZMS_API zn_err_t zms_audio_track_from_g711(zms_audio_track *t, zms_codec_id codec, int sample_rate,
                                            int channels);

/**
 * 尺寸是否明显不可信（常见厂商占位 SPS，如 208x64 / 208x96）。
 * 仅用于决定「后续码流 SPS 可否覆盖」，不强制任何业务分辨率。
 */
ZMS_API int zms_video_size_is_suspicious(uint32_t width, uint32_t height);

/**
 * 是否采纳 new 分辨率。
 * 原则：以码流 SPS 为准；仅拒绝「已有可信尺寸被占位 SPS 回退覆盖」。
 */
ZMS_API int zms_video_size_should_replace(uint32_t cur_w, uint32_t cur_h, uint32_t new_w,
                                          uint32_t new_h);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CORE_STREAM_TRACK_H */
