#ifndef ZM_FORMAT_FLV_H
#define ZM_FORMAT_FLV_H

/**
 * @file flv.h
 * @brief FLV/RTMP 的 ZM reader/writer 接口。
 *
 * reader：输入已拆好的 FLV tag body（RTMP 路径），输出 zm_packet；
 * writer：输入 zm_packet，输出 FLV tag body（RTMP）或完整 tag（HTTP-FLV）。
 */
#include "zm/core/reader.h"
#include "zm/core/writer.h"

/* 接口同时导出 FLV 协议规范（tag 头读写、类型/codec 常量），
 * 使宿主只依赖 zm/format/flv.h，而无需直接 include libflv 的 C 头。 */
#include "flv-demuxer.h"
#include "flv-header.h"
#include "flv-proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建 push 模式 FLV reader；之后用 zm_flv_reader_input_tag 喂 tag body。 */
ZM_API zm_reader *zm_flv_reader_open_push(const zm_reader_cfg *cfg);

/**
 * 创建文件（pull）模式 FLV reader：从可 seek 的 zm_io 顺序读 FLV tag。
 * open 时探测首段以填充 tracks/config；seek 扫到最近视频关键帧。
 * config 只通过 track 暴露（不产出 config 包），供 demuxer 适配层使用。
 */
ZM_API zm_reader *zm_flv_reader_open_file(const zm_io *io, const zm_reader_cfg *cfg);

/**
 * 输入一个 FLV tag body。
 * @param type   8-audio / 9-video / 18-script
 * @param dts_ms tag 时间戳（ms）
 */
ZM_API int zm_flv_reader_input_tag(zm_reader *r, int type, const void *body, size_t len,
                                     uint32_t dts_ms);

/**
 * 创建 FLV writer。
 * cfg->tag_body != 0 时只输出 tag body（RTMP）；否则输出带 FLV 文件头的完整 tag。
 */
ZM_API zm_writer *zm_flv_writer_open(const zm_sink *sink, const zm_writer_cfg *cfg);

/**
 * tag-body 模式的逐 tag 回调（带 FLV type/timestamp）。
 * 设置后优先于 sink；用于把 tag 直接接到 RTMP/HTTP 会话的 outbox。
 */
typedef int (*zm_flv_tag_cb)(void *user, int type, const void *data, size_t len,
                              uint32_t dts_ms);
ZM_API void zm_flv_writer_set_tag_cb(zm_writer *w, zm_flv_tag_cb cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZM_FORMAT_FLV_H */
