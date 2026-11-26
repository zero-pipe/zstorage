#ifndef ZM_CODEC_BITSTREAM_H
#define ZM_CODEC_BITSTREAM_H

/**
 * @file bitstream.h
 * @brief 编解码位流 / config 接口：AVC / HEVC / VVC / AAC / AV1 / VPx。
 *
 * 接口导出 libflv 的 `mpeg4-*` 与 `aom-av1` / `webm-vpx` helper，
 * 使宿主只依赖 `zm/codec/*`，而无需直接 include libflv 的 C 头。
 */
#include "aom-av1.h"
#include "mpeg4-aac.h"
#include "mpeg4-avc.h"
#include "mpeg4-hevc.h"
#include "mpeg4-vvc.h"
#include "webm-vpx.h"

#endif /* ZM_CODEC_BITSTREAM_H */
