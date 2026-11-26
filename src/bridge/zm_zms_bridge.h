#ifndef ZMS_SRC_BRIDGE_ZM_ZMS_BRIDGE_H
#define ZMS_SRC_BRIDGE_ZM_ZMS_BRIDGE_H

/**
 * @file zm_zms_bridge.h
 * @brief ZM 共用模型与 ZMS 现有类型之间的唯一桥接层。
 *
 * 本目录是唯一允许同时认识 zn_buf / zms_frame / zms_gop_slot 和
 * zm_mem / zm_packet / zm_track 的地方。
 */
#include "zm/core/codec.h"
#include "zm/core/mem.h"
#include "zm/core/packet.h"
#include "zm/core/track.h"
#include "zms/core/frame.h"
#include "zms/core/gop/queue.h"
#include "zms/core/media_track.h"
#include "zms/core/stream/hub.h"
#include "zn/util/buf.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 把调用方持有的一份 zn_buf 引用交给 zm_mem。
 * **不额外 ref**：句柄释放时 unref 一次；调用方不要再单独 unref。
 */
zm_mem zms_zm_mem_from_zn(zn_buf *buf);

/** codec id 映射。ZMS 目前只覆盖 H264..H266；超出范围返回 INVALID。 */
zm_codec_id zms_zm_codec_from_zms(zms_codec_id id);
zms_codec_id zms_zm_codec_to_zms(zm_codec_id id);

/** 借用 frame payload，不复制、不转移所有权。 */
void zms_zm_packet_from_frame(const zms_frame *frame, zm_packet *out);

/** 把 packet payload 借给 zms_frame（owned=0）。负时间戳钳到 0。 */
int zms_zm_frame_from_packet(const zm_packet *packet, zms_frame *out);

/** 借用 gop slot payload；slot->pts_ms==0 表示与 dts 相同。 */
void zms_zm_packet_from_gop_slot(const zms_gop_slot *slot, zm_packet *out);

/** 只映射 codec/kind/clock/采样率；不解析 fmtp / config。 */
void zms_zm_track_from_media_track(const zms_media_track *in, zm_track *out);

/**
 * 从 zms_media_source 的视频/音频 config 构建规范化 zm_track（含 config）。
 * 旧 gop 约定：H264=FLV 包裹的 avcC；H265=raw hvcC；AAC=FLV 包裹的 ASC。
 */
int zms_zm_tracks_from_source(const zms_media_source *src, zm_track *tracks,
                               size_t max_tracks, size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_BRIDGE_ZM_ZMS_BRIDGE_H */
