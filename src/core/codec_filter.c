/**
 * @file codec_filter.c
 * @brief 会话能力与角色过滤（play 角色与 codec 交集）。
 */
#include "zcs/core/codec_filter.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/event.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <string.h>

static zcs_codec_id cap_video_codec(const zcs_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZCS_CODEC_INVALID;
    }
    if (s->video.ready && s->video.codec != ZCS_CODEC_INVALID) {
        return s->video.codec;
    }
    if (s->gop_queue) {
        cfg = zcs_gop_queue_video_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zcs_flv_tag_video_codec(cfg, len);
        }
    }
    return ZCS_CODEC_INVALID;
}

static zcs_codec_id cap_audio_codec(const zcs_media_source *s)
{
    size_t len = 0;
    const uint8_t *cfg;

    if (!s) {
        return ZCS_CODEC_INVALID;
    }
    if (s->audio.ready && s->audio.codec != ZCS_CODEC_INVALID) {
        return s->audio.codec;
    }
    if (s->gop_queue) {
        cfg = zcs_gop_queue_audio_config(s->gop_queue, &len);
        if (cfg && len > 0) {
            return zcs_flv_tag_audio_codec(cfg, len);
        }
    }
    return ZCS_CODEC_INVALID;
}

static const zcs_codec_id k_cap_rtsp_video[] = {
    ZCS_CODEC_H264, ZCS_CODEC_H265, ZCS_CODEC_H266, ZCS_CODEC_AV1, ZCS_CODEC_VP8, ZCS_CODEC_VP9,
};

static const zcs_codec_id k_cap_rtsp_audio[] = {
    ZCS_CODEC_AAC,
    ZCS_CODEC_G711A,
    ZCS_CODEC_G711U,
    ZCS_CODEC_OPUS,
};

static const zcs_codec_id k_cap_flv_video[] = {
    ZCS_CODEC_H264,
    ZCS_CODEC_H265,
    ZCS_CODEC_AV1,
};

static const zcs_codec_id k_cap_flv_audio[] = {
    ZCS_CODEC_AAC,
    ZCS_CODEC_G711A,
    ZCS_CODEC_G711U,
};

static const zcs_codec_id k_cap_webrtc_video[] = {
    ZCS_CODEC_H264, ZCS_CODEC_H265, ZCS_CODEC_AV1, ZCS_CODEC_VP8, ZCS_CODEC_VP9,
};

/* 浏览器 WebRTC 播放侧支持 Opus/G711 直通；AAC 等过滤为纯视频 */
static const zcs_codec_id k_cap_webrtc_audio[] = {
    ZCS_CODEC_OPUS,
    ZCS_CODEC_G711A,
    ZCS_CODEC_G711U,
};

static const zcs_codec_id k_cap_ts_video[] = {
    ZCS_CODEC_H264,
    ZCS_CODEC_H265,
};

static const zcs_codec_id k_cap_ts_audio[] = {
    ZCS_CODEC_AAC,
    ZCS_CODEC_G711A,
    ZCS_CODEC_G711U,
};

static const zcs_codec_id k_cap_hls_video[] = {
    ZCS_CODEC_H264, ZCS_CODEC_H265, ZCS_CODEC_H266, ZCS_CODEC_AV1, ZCS_CODEC_VP8, ZCS_CODEC_VP9,
};

static const zcs_codec_id k_cap_hls_audio[] = {
    ZCS_CODEC_AAC,
    ZCS_CODEC_OPUS,
};

typedef struct {
    const zcs_codec_id *video;
    size_t video_n;
    const zcs_codec_id *audio;
    size_t audio_n;
    const char *hint;
} cap_codec_set;

static int cap_codec_in_list(zcs_codec_id codec, const zcs_codec_id *list, size_t n)
{
    size_t i;

    if (codec == ZCS_CODEC_INVALID) {
        return 1;
    }
    for (i = 0; i < n; ++i) {
        if (list[i] == codec) {
            return 1;
        }
    }
    return 0;
}

static const cap_codec_set *cap_sets_for_role(zcs_session_cap_role role)
{
    static const cap_codec_set rtsp = {
        k_cap_rtsp_video,
        sizeof(k_cap_rtsp_video) / sizeof(k_cap_rtsp_video[0]),
        k_cap_rtsp_audio,
        sizeof(k_cap_rtsp_audio) / sizeof(k_cap_rtsp_audio[0]),
        "H264/H265/H266/AV1/VP8/VP9 + AAC/G711/Opus (any combo)",
    };
    static const cap_codec_set flv = {
        k_cap_flv_video,
        sizeof(k_cap_flv_video) / sizeof(k_cap_flv_video[0]),
        k_cap_flv_audio,
        sizeof(k_cap_flv_audio) / sizeof(k_cap_flv_audio[0]),
        "H264/H265/AV1 + AAC/G711 (any combo)",
    };
    static const cap_codec_set webrtc = {
        k_cap_webrtc_video,
        sizeof(k_cap_webrtc_video) / sizeof(k_cap_webrtc_video[0]),
        k_cap_webrtc_audio,
        sizeof(k_cap_webrtc_audio) / sizeof(k_cap_webrtc_audio[0]),
        "H264/H265/AV1/VP8/VP9 + Opus/PCMA/PCMU (AAC 等不支持则自动无声播放)",
    };
    static const cap_codec_set ts = {
        k_cap_ts_video,
        sizeof(k_cap_ts_video) / sizeof(k_cap_ts_video[0]),
        k_cap_ts_audio,
        sizeof(k_cap_ts_audio) / sizeof(k_cap_ts_audio[0]),
        "H264/H265 + AAC/G711 (any combo)",
    };
    static const cap_codec_set hls = {
        k_cap_hls_video,
        sizeof(k_cap_hls_video) / sizeof(k_cap_hls_video[0]),
        k_cap_hls_audio,
        sizeof(k_cap_hls_audio) / sizeof(k_cap_hls_audio[0]),
        "H264/H265/H266/AV1/VP8/VP9 + AAC/Opus (any combo)",
    };

    switch (role) {
    case ZCS_PROTO_CAP_RTSP_PLAY:
    case ZCS_PROTO_CAP_RTSP_PUBLISH:
        return &rtsp;
    case ZCS_PROTO_CAP_RTMP_PLAY:
    case ZCS_PROTO_CAP_RTMP_PUBLISH:
    case ZCS_PROTO_CAP_HTTP_FLV_PLAY:
        return &flv;
    case ZCS_PROTO_CAP_WEBRTC_PLAY:
    case ZCS_PROTO_CAP_WEBRTC_PUBLISH:
        return &webrtc;
    case ZCS_PROTO_CAP_HTTP_TS_PLAY:
    case ZCS_PROTO_CAP_SRT_PLAY:
    case ZCS_PROTO_CAP_SRT_PUBLISH:
    case ZCS_PROTO_CAP_RTP_PS_PUBLISH:
        return &ts;
    case ZCS_PROTO_CAP_HLS_PLAY:
    case ZCS_PROTO_CAP_DASH_PLAY:
        return &hls;
    default:
        return NULL;
    }
}

static int cap_combo_ok(zcs_session_cap_role role, zcs_codec_id vc, zcs_codec_id ac, int has_video,
                        int has_audio)
{
    const cap_codec_set *set = cap_sets_for_role(role);

    if (!set) {
        return 0;
    }
    if (has_video && vc != ZCS_CODEC_INVALID && !cap_codec_in_list(vc, set->video, set->video_n)) {
        return 0;
    }
    if (has_audio && ac != ZCS_CODEC_INVALID && !cap_codec_in_list(ac, set->audio, set->audio_n)) {
        return 0;
    }
    return 1;
}

zn_err_t zcs_session_capability_check(zcs_session_cap_role role, zcs_codec_id video,
                                       zcs_codec_id audio, int has_video, int has_audio)
{
    if (!has_video && !has_audio) {
        return ZN_ERR_INVALID;
    }
    if (!cap_combo_ok(role, video, audio, has_video, has_audio)) {
        return ZN_ERR_NOT_IMPL;
    }
    return ZN_OK;
}

zn_err_t zcs_session_capability_check_source(zcs_session_cap_role role,
                                              const zcs_media_source *src)
{
    zcs_codec_id vc = ZCS_CODEC_INVALID;
    zcs_codec_id ac = ZCS_CODEC_INVALID;
    int hv = 0;
    int ha = 0;

    if (!src) {
        return ZN_ERR_INVALID;
    }
    hv = src->has_video;
    ha = src->has_audio;
    if (hv) {
        vc = cap_video_codec(src);
    }
    if (ha) {
        ac = cap_audio_codec(src);
    }
    if (!hv && !ha) {
        /* WHIP：首 SRTP 后才出现 track；answer 已协商 H264/Opus。 */
        if (src->publishing && src->gop_queue && src->publish_origin == ZCS_ORIGIN_WEBRTC_PUSH) {
            return ZN_OK;
        }
        return ZN_ERR_STATE;
    }
    /* WebRTC 播放：AAC/G711 等浏览器不支持的音轨直接丢掉，允许纯视频 */
    if (role == ZCS_PROTO_CAP_WEBRTC_PLAY && ha && ac != ZCS_CODEC_INVALID) {
        const cap_codec_set *set = cap_sets_for_role(role);
        if (set && !cap_codec_in_list(ac, set->audio, set->audio_n)) {
            zn_info("[capability] webrtc-play filter audio=%s -> video-only app=%s stream=%s",
                     zcs_codec_name(ac), src->app, src->stream);
            ha = 0;
            ac = ZCS_CODEC_INVALID;
        }
    }
    return zcs_session_capability_check(role, vc, ac, hv, ha);
}

zcs_session_cap_role zcs_session_capability_play_role(const char *player)
{
    if (!player || !player[0]) {
        return ZCS_PROTO_CAP_RTMP_PLAY;
    }
    if (strcmp(player, "rtsp") == 0) {
        return ZCS_PROTO_CAP_RTSP_PLAY;
    }
    if (strcmp(player, "http-flv") == 0 || strcmp(player, "ws-flv") == 0) {
        return ZCS_PROTO_CAP_HTTP_FLV_PLAY;
    }
    if (strcmp(player, "http-ts") == 0) {
        return ZCS_PROTO_CAP_HTTP_TS_PLAY;
    }
    if (strcmp(player, "webrtc") == 0 || strcmp(player, "whep") == 0) {
        return ZCS_PROTO_CAP_WEBRTC_PLAY;
    }
    if (strcmp(player, "srt") == 0) {
        return ZCS_PROTO_CAP_SRT_PLAY;
    }
    if (strcmp(player, "hls") == 0) {
        return ZCS_PROTO_CAP_HLS_PLAY;
    }
    if (strcmp(player, "dash") == 0) {
        return ZCS_PROTO_CAP_DASH_PLAY;
    }
    return ZCS_PROTO_CAP_RTMP_PLAY;
}

zcs_session_cap_role zcs_session_capability_publish_role(const zcs_media_source *src)
{
    if (!src) {
        return ZCS_PROTO_CAP_RTMP_PUBLISH;
    }
    if (src->publish_origin == ZCS_ORIGIN_SRT_PUSH) {
        return ZCS_PROTO_CAP_SRT_PUBLISH;
    }
    if (src->publish_origin == ZCS_ORIGIN_RTP_PS_PUSH) {
        return ZCS_PROTO_CAP_RTP_PS_PUBLISH;
    }
    if (src->publish_origin == ZCS_ORIGIN_RTSP_PUSH) {
        return ZCS_PROTO_CAP_RTSP_PUBLISH;
    }
    if (src->publish_origin == ZCS_ORIGIN_WEBRTC_PUSH) {
        return ZCS_PROTO_CAP_WEBRTC_PUBLISH;
    }
    if (strcmp(src->schema, ZCS_SCHEMA_SRT) == 0) {
        return ZCS_PROTO_CAP_SRT_PUBLISH;
    }
    if (strcmp(src->schema, ZCS_SCHEMA_RTP_PS) == 0) {
        return ZCS_PROTO_CAP_RTP_PS_PUBLISH;
    }
    if (strcmp(src->schema, ZCS_SCHEMA_RTSP) == 0) {
        return ZCS_PROTO_CAP_RTSP_PUBLISH;
    }
    return ZCS_PROTO_CAP_RTMP_PUBLISH;
}

zn_err_t zcs_session_capability_check_publish_source(const zcs_media_source *src)
{
    return zcs_session_capability_check_source(zcs_session_capability_publish_role(src), src);
}

const char *zcs_session_capability_role_name(zcs_session_cap_role role)
{
    switch (role) {
    case ZCS_PROTO_CAP_RTSP_PLAY:
        return "rtsp-play";
    case ZCS_PROTO_CAP_RTSP_PUBLISH:
        return "rtsp-publish";
    case ZCS_PROTO_CAP_RTMP_PLAY:
        return "rtmp-play";
    case ZCS_PROTO_CAP_RTMP_PUBLISH:
        return "rtmp-publish";
    case ZCS_PROTO_CAP_HTTP_FLV_PLAY:
        return "http-flv-play";
    case ZCS_PROTO_CAP_HTTP_TS_PLAY:
        return "http-ts-play";
    case ZCS_PROTO_CAP_WEBRTC_PLAY:
        return "webrtc-play";
    case ZCS_PROTO_CAP_WEBRTC_PUBLISH:
        return "webrtc-publish";
    case ZCS_PROTO_CAP_SRT_PLAY:
        return "srt-play";
    case ZCS_PROTO_CAP_SRT_PUBLISH:
        return "srt-publish";
    case ZCS_PROTO_CAP_RTP_PS_PUBLISH:
        return "rtp-ps-publish";
    case ZCS_PROTO_CAP_HLS_PLAY:
        return "hls-play";
    case ZCS_PROTO_CAP_DASH_PLAY:
        return "dash-play";
    default:
        return "unknown";
    }
}

const char *zcs_session_capability_allowed_hint(zcs_session_cap_role role)
{
    const cap_codec_set *set = cap_sets_for_role(role);

    return set && set->hint ? set->hint : "";
}

void zcs_session_capability_log_reject(const char *player, const zcs_media_source *src,
                                       zcs_session_cap_role role)
{
    zcs_codec_id vc = ZCS_CODEC_INVALID;
    zcs_codec_id ac = ZCS_CODEC_INVALID;

    if (!src) {
        return;
    }
    if (src->has_video) {
        vc = cap_video_codec(src);
    }
    if (src->has_audio) {
        ac = cap_audio_codec(src);
    }
    zn_warn("[capability] reject %s app=%s stream=%s video=%s audio=%s allowed=%s",
             player && player[0] ? player : zcs_session_capability_role_name(role), src->app,
             src->stream, vc != ZCS_CODEC_INVALID ? zcs_codec_name(vc) : "-",
             ac != ZCS_CODEC_INVALID ? zcs_codec_name(ac) : "-",
             zcs_session_capability_allowed_hint(role));
}
