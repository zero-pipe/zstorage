#ifndef ZM_FORMAT_MPEG_H
#define ZM_FORMAT_MPEG_H

/**
 * @file mpeg.h
 * @brief MPEG-TS / PS 的 ZM reader/writer 接口。
 *
 * cfg->format 选择 ZM_FMT_MPEG_TS 或 ZM_FMT_MPEG_PS。
 * 当前实现 TS；PS 返回 NULL（后续补）。
 */
#include "zm/core/reader.h"
#include "zm/core/writer.h"

/* 接口同时导出 MPEG-TS/PS 的协议规范头与 stream_type，供宿主直接使用。 */
#include "mpeg-proto.h"
#include "mpeg-ps.h"
#include "mpeg-ts.h"
#include "mpeg-types.h"

#ifdef __cplusplus
extern "C" {
#endif

ZM_API zm_reader *zm_mpeg_reader_open(const zm_reader_cfg *cfg);
ZM_API zm_writer *zm_mpeg_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* ZM_FORMAT_MPEG_H */
