/**
 * @file flv_tag_probe.h
 * @brief FLV tag codec/metadata 探测。
 */
#ifndef ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_PROBE_H
#define ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_PROBE_H

/**
 * FLV tag body 只读探测：codec 识别、seq header / raw 分类、AAC ASC 定位。
 * 不负责 ES 解复用（见 flv_tag_demuxer）或 codec 打包（见 {codec}_over_rtmp）。
 */
#include "zcs/media/codec/codec_id.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** FLV VideoTag / AudioTag 的 AVCPacketType / AACPacketType 语义。 */
typedef enum {
    ZCS_FLV_TAG_PKT_INVALID = -1,
    ZCS_FLV_TAG_PKT_RAW = 0,
    ZCS_FLV_TAG_PKT_SEQ_HEADER = 1,
    ZCS_FLV_TAG_PKT_END_OF_SEQ = 2,
    ZCS_FLV_TAG_PKT_OTHER = 3,
} zcs_flv_tag_packet_kind;

/** 判定 video tag 的 AVCPacketType 语义（seq header / raw / end-of-seq / other）；输入非法返回 INVALID。 */
ZCS_API zcs_flv_tag_packet_kind zcs_flv_tag_video_packet_kind(const uint8_t *tag, size_t len);
/** 判定 audio tag 语义；仅 AAC/Opus 区分 seq header，其余编解码一律按 RAW 处理。 */
ZCS_API zcs_flv_tag_packet_kind zcs_flv_tag_audio_packet_kind(const uint8_t *tag, size_t len);

/** 从 video tag 头解析编解码 id；无法识别返回 ZCS_CODEC_INVALID。 */
ZCS_API zcs_codec_id zcs_flv_tag_video_codec(const uint8_t *tag, size_t len);
/** 从 audio tag 头解析编解码 id；无法识别返回 ZCS_CODEC_INVALID。 */
ZCS_API zcs_codec_id zcs_flv_tag_audio_codec(const uint8_t *tag, size_t len);

/** avcC/hvcc 或 FLV seq header：识别 video config 的 codec。 */
ZCS_API zcs_codec_id zcs_flv_video_config_codec(const uint8_t *cfg, size_t len);

/** AAC sequence-header tag：指向 AudioSpecificConfig 字节。 */
ZCS_API zn_err_t zcs_flv_tag_aac_seq_header_asc(const uint8_t *tag, size_t len,
                                                 const uint8_t **asc, size_t *asc_len);

/** 剥 FLV audio tag 头，得到 AAC/G711 ES。 */
ZCS_API zn_err_t zcs_flv_tag_audio_to_es(const uint8_t *body, size_t len, const uint8_t **es,
                                          size_t *es_len, zcs_codec_id *codec);

/** onMetaData videocodecid：H.264=7，Enhanced H.265='hvc1'(1752589105) */
ZCS_API double zcs_flv_metadata_videocodecid(zcs_codec_id id);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_MEDIA_CONTAINER_FLV_FLV_TAG_PROBE_H */
