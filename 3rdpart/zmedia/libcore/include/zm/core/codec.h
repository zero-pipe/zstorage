#ifndef ZM_CORE_CODEC_H
#define ZM_CORE_CODEC_H

/**
 * @file codec.h
 * @brief 唯一 codec 注册表：codec 是数据，不是文件。
 *
 * 一张静态表取代 FLV / PSI / MOV / MKV / RTP 五套 ID 空间；
 * 各格式库只通过 zm_codec_from_* 做一次映射。
 */
#include "zm/core/types.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zm_codec_desc {
    zm_codec_id id;
    zm_kind kind;
    const char *name;
    int clock_hz;
    struct {
        int flv_video;        /**< FLV_VIDEO_*；0 = 不支持 */
        int flv_audio;        /**< FLV_AUDIO_*；0 = 不支持 */
        int psi;              /**< PSI_STREAM_*；0 = 不支持 */
        int mov_object;       /**< MOV_OBJECT_*；0 = 不支持 */
        int mkv_codec;        /**< enum mkv_codec_t；0 = 不支持 */
        int rtp_payload;      /**< 默认 RTP payload type；0 = 动态 */
        const char *rtp_encoding; /**< rtpmap encoding name */
        const char *sdp_mime;
    } map;
} zm_codec_desc;

ZM_API const zm_codec_desc *zm_codec_get(zm_codec_id id);
ZM_API const char *zm_codec_name(zm_codec_id id);
ZM_API zm_kind zm_codec_kind(zm_codec_id id);
ZM_API int zm_codec_clock_hz(zm_codec_id id);
ZM_API unsigned zm_codec_count(void);

ZM_API zm_codec_id zm_codec_from_name(const char *name);
ZM_API zm_codec_id zm_codec_from_flv(int flv_codec);
ZM_API zm_codec_id zm_codec_from_psi(int psi);
ZM_API zm_codec_id zm_codec_from_mov(int object_type);
ZM_API zm_codec_id zm_codec_from_mkv(int mkv_codec);
ZM_API zm_codec_id zm_codec_from_rtpmap(const char *encoding, int clock_hz);

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_CODEC_H */
