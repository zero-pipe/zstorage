/**
 * @file live.h
 * @brief 领域聚合头：直播入站、代理拉流与 codec/容器适配。
 */
#ifndef ZMS_LIVE_H
#define ZMS_LIVE_H

/** 直播入站、推流辅助、代理拉流与通用 codec/容器适配器。 */
#include "zms/publish/ingest/common/protocol_opts.h"
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/pull/proxy.h"
#include "zms/media/codec/codec_id.h"
#include "zms/media/codec/bitstream/annexb.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h264/h264_sps.h"
#include "zms/media/wire_format.h"
#include "zms/media/container/flv/flv_tag_probe.h"
#include "zms/media/codec/h264/h264_over_rtmp.h"
#include "zms/media/codec/aac/aac_over_rtp.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/g711/g711_over_rtp.h"
#include "zms/media/codec/g711/g711_over_rtmp.h"
#include "zms/media/container/flv/flv_tag_framer.h"
#include "zms/play/http_flv/live_muxer.h"
#include "zms/media/container/flv/flv_tag_demuxer.h"

#endif /* ZMS_LIVE_H */
