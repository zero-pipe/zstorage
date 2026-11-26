/**
 * @file av1_over_rtmp.h
 * @brief AV1 在 RTMP/FLV 的 config 与打包。
 */
#ifndef ZMS_MEDIA_CODEC_AV1_AV1_OVER_RTMP_H
#define ZMS_MEDIA_CODEC_AV1_AV1_OVER_RTMP_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 从 FLV 视频 tag（AV1 序列头）或裸 av1C 中定位 AV1CodecConfigurationRecord。
 * @param av1c 输出借用视图（指向 data）
 * @return 1 成功；0 参数非法或无数据。
 */
ZMS_API int zms_av1_over_rtmp_config_extradata(const uint8_t *data, size_t len,
                                               const uint8_t **av1c, size_t *av1c_len);

/**
 * 生成 FLV AV1 视频序列头 tag（含 av1C），已是序列头时直接拷贝。
 * @return ZN_OK 成功；参数非法或 BUFFER_TOO_SMALL 失败。
 */
ZMS_API zn_err_t zms_av1_flv_sequence_header(const uint8_t *cfg, size_t cfg_len, uint8_t *out,
                                              size_t cap, size_t *out_len);

/**
 * 将一个 AV1 OBU 访问单元打为 FLV 视频 tag（enhanced RTMP）。
 * @param key 非 0 标记关键帧；video_cfg 当前未使用
 */
ZMS_API zn_err_t zms_av1_over_rtmp_pack_es(const uint8_t *video_cfg, size_t cfg_len,
                                            const uint8_t *obu, size_t len, int key, uint8_t *out,
                                            size_t cap, size_t *out_len);

/** 从 AV1 OBU 访问单元生成 av1c extradata；成功返回写入字节数 */
ZMS_API int zms_av1_extradata_from_obu(const uint8_t *obu, size_t len, uint8_t *out, size_t cap,
                                       int *width, int *height);

/** OBU 链中是否含 Sequence Header（OBU type 1，AV1 RTP coded video sequence 起点） */
ZMS_API int zms_av1_obu_has_sequence_header(const uint8_t *obu, size_t len);

/** 是否含 Sequence Header 且可解析出宽高，可作为 DASH/HLS 切段 sync 点 */
ZMS_API int zms_av1_obu_is_sync_key(const uint8_t *obu, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_AV1_AV1_OVER_RTMP_H */
