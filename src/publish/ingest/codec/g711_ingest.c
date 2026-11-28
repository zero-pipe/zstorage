/**
 * @file g711_ingest.c
 * @brief G.711 入站。
 */
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/publish/ingest/common/ingest_codec.h"
#include "publish/ingest/common/ingest_internal.h"
#include "zn/util/log.h"

void zcs_live_ingest_g711_ensure(zcs_live_ingest *ch, zcs_codec_id codec)
{
    if (!ch || ch->have_audio_cfg) {
        return;
    }
    int rate = 8000;
    zcs_media_timeline_set_audio(&ch->tl, codec, (uint32_t)rate);
    ch->have_audio_cfg = 1;
    ch->source->has_audio = 1;
    /* 必须标记真实音轨：否则 live_ingest_maybe_mute_audio 会继续往 GOP 队列里
     * 注入静音 AAC，混进 G.711 流里被当作 G.711 打包（每 1024 采样一次杂波）。 */
    zcs_live_ingest_mark_real_audio(ch);
    (void)zcs_audio_track_from_g711(&ch->source->audio, codec, rate, 1);
    zn_info("ingress: G711 audio ready codec=%s", zcs_codec_name(codec));
}

zn_err_t zcs_live_ingest_input_g711_es(zcs_live_ingest *ch, zcs_codec_id codec,
                                        const uint8_t *g711, size_t len, uint32_t dts_ms)
{
    if (!ch || !g711 || len == 0 || (codec != ZCS_CODEC_G711A && codec != ZCS_CODEC_G711U)) {
        return ZN_ERR_INVALID;
    }

    zcs_live_ingest_g711_ensure(ch, codec);
    dts_ms = live_ingest_audio_pts(ch, dts_ms);

    if (!ch->source->gop_queue) {
        return ZN_ERR_INVALID;
    }
    zcs_frame frame;
    zcs_frame_init(&frame);
    frame.data = (uint8_t *)g711;
    frame.size = len;
    frame.dts_ms = frame.pts_ms = dts_ms;
    frame.codec = codec;
    frame.track = ZCS_TRACK_AUDIO;
    live_ingest_write_frame(ch, &frame);
    ch->source->has_audio = 1;
    return ZN_OK;
}
