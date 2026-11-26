#ifndef ZMS_MEDIA_CODEC_BITSTREAM_ANNEXB_H
#define ZMS_MEDIA_CODEC_BITSTREAM_ANNEXB_H

/**
 * @file annexb.h
 * @brief Annex-B NAL 查找（H.26x 字节流辅助）。
 */
#include "zms/zms_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 在 [p, end) 内查找第一个 NAL 单元（跳过起始码）。
 * @param nal_len 输出 NAL 字节数（不含起始码）
 * @return 借用的 NAL 起始指针（指向入参缓冲）；未找到或参数非法返回 NULL
 */
ZMS_API const uint8_t *zms_annexb_find_nal(const uint8_t *p, const uint8_t *end, size_t *nal_len);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_CODEC_BITSTREAM_ANNEXB_H */
