/**
 * @file live.h
 * @brief 领域聚合头：直播入站、代理拉流与 codec/容器适配。
 */
#ifndef ZCS_LIVE_H
#define ZCS_LIVE_H

/** 直播入站、推流辅助、代理拉流与通用 codec/容器适配器。 */
#include "zcs/publish/ingest/common/protocol_opts.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/pull/proxy.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/codec/bitstream/annexb.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h264/h264_sps.h"
#include "zcs/media/wire_format.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/media/codec/h264/h264_over_rtmp.h"
#include "zcs/media/codec/aac/aac_over_rtp.h"
#include "zcs/media/codec/aac/aac_over_rtmp.h"
#include "zcs/media/codec/g711/g711_over_rtp.h"
#include "zcs/media/codec/g711/g711_over_rtmp.h"
#include "zcs/media/container/flv/flv_tag_framer.h"
#include "zcs/media/container/flv/flv_tag_demuxer.h"

#endif /* ZCS_LIVE_H */
