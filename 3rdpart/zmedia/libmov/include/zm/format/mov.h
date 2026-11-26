#ifndef ZM_FORMAT_MOV_H
#define ZM_FORMAT_MOV_H

/**
 * @file mov.h
 * @brief MP4/MOV/fMP4 的 ZM reader 接口（pull 模式，需要可 seek 的 zm_io）。
 *
 * 视频样本在 reader 边界统一转成 Annex-B；AAC 保持 raw。
 */
#include "zm/core/io.h"
#include "zm/core/reader.h"
#include "zm/core/writer.h"

/* 接口同时导出 MOV 协议规范常量（MOV_OBJECT_xxx、MOV_AV_FLAG_xxx）与文件缓冲接口。 */
#include "mov-buffer.h"
#include "mov-format.h"

#ifdef __cplusplus
extern "C" {
#endif

ZM_API zm_reader *zm_mov_reader_open(const zm_io *io, const zm_reader_cfg *cfg);

/**
 * 创建 MP4/fMP4 writer。
 * cfg->format = ZM_FMT_MOV 输出普通 MP4（sink 必须支持 seek/tell）；
 * cfg->format = ZM_FMT_FMP4 输出 fragmented MP4（适合网络 sink）。
 */
ZM_API zm_writer *zm_mov_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* ZM_FORMAT_MOV_H */
