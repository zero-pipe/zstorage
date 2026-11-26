#ifndef ZM_FORMAT_MKV_H
#define ZM_FORMAT_MKV_H

/**
 * @file mkv.h
 * @brief Matroska 的 ZM reader 接口（pull 模式，需要可 seek 的 zm_io）。
 *
 * 视频样本在 reader 边界统一转成 Annex-B；AAC/Opus 保持 raw。
 */
#include "zm/core/io.h"
#include "zm/core/reader.h"
#include "zm/core/writer.h"

/* 接口同时导出 MKV 文件缓冲接口（mkv_buffer_t）。 */
#include "mkv-buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

ZM_API zm_reader *zm_mkv_reader_open(const zm_io *io, const zm_reader_cfg *cfg);
ZM_API zm_writer *zm_mkv_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* ZM_FORMAT_MKV_H */
