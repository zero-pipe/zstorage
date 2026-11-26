/**
 * @file g711_ingest.c
 * @brief G.711 入站。
 */
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/publish/ingest/common/ingest_codec.h"
#include "publish/ingest/common/ingest_internal.h"
#include "zn/util/log.h"

void zms_live_ingest_g711_ensure(zms_live_ingest *ch, zms_codec_id codec)
{
    if (!ch || ch->have_audio_cfg) {
        return;
    }
    int rate = 8000;
    zms_media_timeline_set_audio(&ch->tl, codec, (uint32_t)rate);
    ch->have_audio_cfg = 1;
    ch->source->has_audio = 1;
    /* 必须标记真实音轨：否则 live_ingest_maybe_mute_audio 会继续往 GOP 队列里
     * 注入静音 AAC，混进 G.711 流里被当作 G.711 打包（每 1024 采样一次杂波）。 */
    zms_live_ingest_mark_real_audio(ch);
    (void)zms_audio_track_from_g711(&ch->source->audio, codec, rate, 1);
    zn_info("ingress: G711 audio ready codec=%s", zms_codec_name(codec));
}

zn_err_t zms_live_ingest_input_g711_es(zms_live_ingest *ch, zms_codec_id codec,
                                        const uint8_t *g711, size_t len, uint32_t dts_ms)
{
    if (!ch || !g711 || len == 0 || (codec != ZMS_CODEC_G711A && codec != ZMS_CODEC_G711U)) {
        return ZN_ERR_INVALID;
    }

    zms_live_ingest_g711_ensure(ch, codec);
    dts_ms = live_ingest_audio_pts(ch, dts_ms);

    if (!ch->source->gop_queue) {
        return ZN_ERR_INVALID;
    }
    zms_frame frame;
    zms_frame_init(&frame);
    frame.data = (uint8_t *)g711;
    frame.size = len;
    frame.dts_ms = frame.pts_ms = dts_ms;
    frame.codec = codec;
    frame.track = ZMS_TRACK_AUDIO;
    live_ingest_write_frame(ch, &frame);
    ch->source->has_audio = 1;
    return ZN_OK;
}
