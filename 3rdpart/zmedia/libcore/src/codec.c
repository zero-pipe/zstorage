#include "zm/core/codec.h"

#include <ctype.h>
#include <string.h>

/*
 * 各格式的 ID 数值在这里显式写死，避免 libcore 反向 include 容器头。
 * 数值来自：
 *   libflv/include/flv-proto.h
 *   libmpeg/include/mpeg-proto.h
 *   libmov/include/mov-format.h
 *   libmkv/include/mkv-format.h
 *   librtp/include/rtp-profile.h
 */
#define ZM_FLV_AUDIO_MP3 (2 << 4)
#define ZM_FLV_AUDIO_EAC3 (5 << 4)
#define ZM_FLV_AUDIO_G711A (7 << 4)
#define ZM_FLV_AUDIO_G711U (8 << 4)
#define ZM_FLV_AUDIO_AAC (10 << 4)
#define ZM_FLV_AUDIO_AC3 (12 << 4)
#define ZM_FLV_AUDIO_OPUS (13 << 4)
#define ZM_FLV_VIDEO_H264 7
#define ZM_FLV_VIDEO_H265 12
#define ZM_FLV_VIDEO_AV1 13
#define ZM_FLV_VIDEO_H266 15
#define ZM_FLV_VIDEO_VP8 16
#define ZM_FLV_VIDEO_VP9 17

#define ZM_PSI_MP3 0x04
#define ZM_PSI_AAC 0x0f
#define ZM_PSI_H264 0x1b
#define ZM_PSI_H265 0x24
#define ZM_PSI_H266 0x33
#define ZM_PSI_G711A 0x90
#define ZM_PSI_G711U 0x91
#define ZM_PSI_OPUS 0x9c
#define ZM_PSI_VP8 0x9d
#define ZM_PSI_VP9 0x9e
#define ZM_PSI_AV1 0x9f
#define ZM_PSI_AC3 0x81
#define ZM_PSI_EAC3 0x87

#define ZM_MOV_H264 0x21
#define ZM_MOV_H265 0x23
#define ZM_MOV_AAC 0x40
#define ZM_MOV_AAC_MAIN 0x66
#define ZM_MOV_AAC_LC 0x67
#define ZM_MOV_AAC_SSR 0x68
#define ZM_MOV_MP3 0x69
#define ZM_MOV_AC3 0xA5
#define ZM_MOV_EAC3 0xA6
#define ZM_MOV_OPUS 0xAD
#define ZM_MOV_VP9 0xB1
#define ZM_MOV_VP8 0xC2
#define ZM_MOV_H266 0xFC
#define ZM_MOV_G711A 0xFD
#define ZM_MOV_G711U 0xFE
#define ZM_MOV_AV1 0xFF

#define ZM_MKV_H264 9
#define ZM_MKV_H265 10
#define ZM_MKV_VP8 18
#define ZM_MKV_VP9 19
#define ZM_MKV_AV1 21
#define ZM_MKV_H266 24
#define ZM_MKV_MP3 4096
#define ZM_MKV_AC3 4103
#define ZM_MKV_FLAC 4109
#define ZM_MKV_AAC 4117
#define ZM_MKV_OPUS 4121
#define ZM_MKV_EAC3 4123

#define ZM_RTP_PCMU 0
#define ZM_RTP_PCMA 8
#define ZM_RTP_MP3 14
#define ZM_RTP_H264 98
#define ZM_RTP_H265 100
#define ZM_RTP_MP4A 101
#define ZM_RTP_OPUS 103
#define ZM_RTP_VP8 105
#define ZM_RTP_VP9 106
#define ZM_RTP_AV1 107
#define ZM_RTP_H266 108

static const zm_codec_desc k_codecs[] = {
    {.id = ZM_CODEC_H264,
     .kind = ZM_KIND_VIDEO,
     .name = "H264",
     .clock_hz = 90000,
     .map = {.flv_video = ZM_FLV_VIDEO_H264,
             .psi = ZM_PSI_H264,
             .mov_object = ZM_MOV_H264,
             .mkv_codec = ZM_MKV_H264,
             .rtp_payload = ZM_RTP_H264,
             .rtp_encoding = "H264",
             .sdp_mime = "H264"}},
    {.id = ZM_CODEC_H265,
     .kind = ZM_KIND_VIDEO,
     .name = "H265",
     .clock_hz = 90000,
     .map = {.flv_video = ZM_FLV_VIDEO_H265,
             .psi = ZM_PSI_H265,
             .mov_object = ZM_MOV_H265,
             .mkv_codec = ZM_MKV_H265,
             .rtp_payload = ZM_RTP_H265,
             .rtp_encoding = "H265",
             .sdp_mime = "H265"}},
    {.id = ZM_CODEC_H266,
     .kind = ZM_KIND_VIDEO,
     .name = "H266",
     .clock_hz = 90000,
     .map = {.flv_video = ZM_FLV_VIDEO_H266,
             .psi = ZM_PSI_H266,
             .mov_object = ZM_MOV_H266,
             .mkv_codec = ZM_MKV_H266,
             .rtp_payload = ZM_RTP_H266,
             .rtp_encoding = "H266",
             .sdp_mime = "H266"}},
    {.id = ZM_CODEC_AV1,
     .kind = ZM_KIND_VIDEO,
     .name = "AV1",
     .clock_hz = 90000,
     .map = {.flv_video = ZM_FLV_VIDEO_AV1,
             .psi = ZM_PSI_AV1,
             .mov_object = ZM_MOV_AV1,
             .mkv_codec = ZM_MKV_AV1,
             .rtp_payload = ZM_RTP_AV1,
             .rtp_encoding = "AV1",
             .sdp_mime = "AV1"}},
    {.id = ZM_CODEC_VP8,
     .kind = ZM_KIND_VIDEO,
     .name = "VP8",
     .clock_hz = 90000,
     .map = {.flv_video = ZM_FLV_VIDEO_VP8,
             .psi = ZM_PSI_VP8,
             .mov_object = ZM_MOV_VP8,
             .mkv_codec = ZM_MKV_VP8,
             .rtp_payload = ZM_RTP_VP8,
             .rtp_encoding = "VP8",
             .sdp_mime = "VP8"}},
    {.id = ZM_CODEC_VP9,
     .kind = ZM_KIND_VIDEO,
     .name = "VP9",
     .clock_hz = 90000,
     .map = {.flv_video = ZM_FLV_VIDEO_VP9,
             .psi = ZM_PSI_VP9,
             .mov_object = ZM_MOV_VP9,
             .mkv_codec = ZM_MKV_VP9,
             .rtp_payload = ZM_RTP_VP9,
             .rtp_encoding = "VP9",
             .sdp_mime = "VP9"}},
    {.id = ZM_CODEC_AAC,
     .kind = ZM_KIND_AUDIO,
     .name = "AAC",
     .clock_hz = 44100,
     .map = {.flv_audio = ZM_FLV_AUDIO_AAC,
             .psi = ZM_PSI_AAC,
             .mov_object = ZM_MOV_AAC,
             .mkv_codec = ZM_MKV_AAC,
             .rtp_payload = ZM_RTP_MP4A,
             .rtp_encoding = "mpeg4-generic",
             .sdp_mime = "mpeg4-generic"}},
    {.id = ZM_CODEC_OPUS,
     .kind = ZM_KIND_AUDIO,
     .name = "Opus",
     .clock_hz = 48000,
     .map = {.flv_audio = ZM_FLV_AUDIO_OPUS,
             .psi = ZM_PSI_OPUS,
             .mov_object = ZM_MOV_OPUS,
             .mkv_codec = ZM_MKV_OPUS,
             .rtp_payload = ZM_RTP_OPUS,
             .rtp_encoding = "opus",
             .sdp_mime = "opus"}},
    {.id = ZM_CODEC_G711A,
     .kind = ZM_KIND_AUDIO,
     .name = "G711A",
     .clock_hz = 8000,
     .map = {.flv_audio = ZM_FLV_AUDIO_G711A,
             .psi = ZM_PSI_G711A,
             .mov_object = ZM_MOV_G711A,
             .rtp_payload = ZM_RTP_PCMA,
             .rtp_encoding = "PCMA",
             .sdp_mime = "PCMA"}},
    {.id = ZM_CODEC_G711U,
     .kind = ZM_KIND_AUDIO,
     .name = "G711U",
     .clock_hz = 8000,
     .map = {.flv_audio = ZM_FLV_AUDIO_G711U,
             .psi = ZM_PSI_G711U,
             .mov_object = ZM_MOV_G711U,
             .rtp_payload = ZM_RTP_PCMU,
             .rtp_encoding = "PCMU",
             .sdp_mime = "PCMU"}},
    {.id = ZM_CODEC_MP3,
     .kind = ZM_KIND_AUDIO,
     .name = "MP3",
     .clock_hz = 44100,
     .map = {.flv_audio = ZM_FLV_AUDIO_MP3,
             .psi = ZM_PSI_MP3,
             .mov_object = ZM_MOV_MP3,
             .mkv_codec = ZM_MKV_MP3,
             .rtp_payload = ZM_RTP_MP3,
             .rtp_encoding = "MPA",
             .sdp_mime = "MPA"}},
    {.id = ZM_CODEC_AC3,
     .kind = ZM_KIND_AUDIO,
     .name = "AC3",
     .clock_hz = 48000,
     .map = {.flv_audio = ZM_FLV_AUDIO_AC3,
             .psi = ZM_PSI_AC3,
             .mov_object = ZM_MOV_AC3,
             .mkv_codec = ZM_MKV_AC3,
             .rtp_encoding = "AC3",
             .sdp_mime = "AC3"}},
    {.id = ZM_CODEC_EAC3,
     .kind = ZM_KIND_AUDIO,
     .name = "EAC3",
     .clock_hz = 48000,
     .map = {.flv_audio = ZM_FLV_AUDIO_EAC3,
             .psi = ZM_PSI_EAC3,
             .mov_object = ZM_MOV_EAC3,
             .mkv_codec = ZM_MKV_EAC3,
             .rtp_encoding = "EAC3",
             .sdp_mime = "EAC3"}},
    {.id = ZM_CODEC_FLAC,
     .kind = ZM_KIND_AUDIO,
     .name = "FLAC",
     .clock_hz = 48000,
     .map = {.mkv_codec = ZM_MKV_FLAC, .rtp_encoding = "FLAC", .sdp_mime = "FLAC"}},
};

static int ci_equal(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

const zm_codec_desc *zm_codec_get(zm_codec_id id)
{
    size_t i;

    for (i = 0; i < sizeof(k_codecs) / sizeof(k_codecs[0]); ++i) {
        if (k_codecs[i].id == id) {
            return &k_codecs[i];
        }
    }
    return NULL;
}

const char *zm_codec_name(zm_codec_id id)
{
    const zm_codec_desc *d = zm_codec_get(id);
    return d ? d->name : "invalid";
}

zm_kind zm_codec_kind(zm_codec_id id)
{
    const zm_codec_desc *d = zm_codec_get(id);
    return d ? d->kind : ZM_KIND_UNKNOWN;
}

int zm_codec_clock_hz(zm_codec_id id)
{
    const zm_codec_desc *d = zm_codec_get(id);
    return d ? d->clock_hz : 0;
}

unsigned zm_codec_count(void)
{
    return (unsigned)(sizeof(k_codecs) / sizeof(k_codecs[0]));
}

zm_codec_id zm_codec_from_name(const char *name)
{
    size_t i;

    if (!name || !name[0]) {
        return ZM_CODEC_INVALID;
    }
    if (ci_equal(name, "H.264") || ci_equal(name, "AVC")) {
        return ZM_CODEC_H264;
    }
    if (ci_equal(name, "H.265") || ci_equal(name, "HEVC")) {
        return ZM_CODEC_H265;
    }
    if (ci_equal(name, "H.266") || ci_equal(name, "VVC")) {
        return ZM_CODEC_H266;
    }
    if (ci_equal(name, "MPA")) {
        return ZM_CODEC_MP3;
    }
    for (i = 0; i < sizeof(k_codecs) / sizeof(k_codecs[0]); ++i) {
        if (ci_equal(name, k_codecs[i].name) || ci_equal(name, k_codecs[i].map.sdp_mime)) {
            return k_codecs[i].id;
        }
    }
    return ZM_CODEC_INVALID;
}

zm_codec_id zm_codec_from_flv(int flv_codec)
{
    size_t i;

    if (!flv_codec) {
        return ZM_CODEC_INVALID;
    }
    for (i = 0; i < sizeof(k_codecs) / sizeof(k_codecs[0]); ++i) {
        if (k_codecs[i].map.flv_video == flv_codec || k_codecs[i].map.flv_audio == flv_codec) {
            return k_codecs[i].id;
        }
    }
    return ZM_CODEC_INVALID;
}

zm_codec_id zm_codec_from_psi(int psi)
{
    size_t i;

    for (i = 0; i < sizeof(k_codecs) / sizeof(k_codecs[0]); ++i) {
        if (k_codecs[i].map.psi == psi && psi != 0) {
            return k_codecs[i].id;
        }
    }
    switch (psi) {
    case 0x25:
        return ZM_CODEC_H265;
    case 0x34:
        return ZM_CODEC_H266;
    case 0x11:
    case 0x1c:
        return ZM_CODEC_AAC;
    default:
        return ZM_CODEC_INVALID;
    }
}

zm_codec_id zm_codec_from_mov(int object_type)
{
    size_t i;

    for (i = 0; i < sizeof(k_codecs) / sizeof(k_codecs[0]); ++i) {
        if (k_codecs[i].map.mov_object == object_type && object_type != 0) {
            return k_codecs[i].id;
        }
    }
    switch (object_type) {
    case ZM_MOV_AAC_MAIN:
    case ZM_MOV_AAC_LC:
    case ZM_MOV_AAC_SSR:
        return ZM_CODEC_AAC;
    default:
        return ZM_CODEC_INVALID;
    }
}

zm_codec_id zm_codec_from_mkv(int mkv_codec)
{
    size_t i;

    for (i = 0; i < sizeof(k_codecs) / sizeof(k_codecs[0]); ++i) {
        if (k_codecs[i].map.mkv_codec == mkv_codec && mkv_codec != 0) {
            return k_codecs[i].id;
        }
    }
    return ZM_CODEC_INVALID;
}

zm_codec_id zm_codec_from_rtpmap(const char *encoding, int clock_hz)
{
    size_t i;

    (void)clock_hz;
    if (!encoding || !encoding[0]) {
        return ZM_CODEC_INVALID;
    }
    if (ci_equal(encoding, "AVC")) {
        return ZM_CODEC_H264;
    }
    if (ci_equal(encoding, "HEVC")) {
        return ZM_CODEC_H265;
    }
    if (ci_equal(encoding, "VVC")) {
        return ZM_CODEC_H266;
    }
    if (ci_equal(encoding, "AV1X")) {
        return ZM_CODEC_AV1;
    }
    if (ci_equal(encoding, "MP3") || ci_equal(encoding, "MPA")) {
        return ZM_CODEC_MP3;
    }
    if (ci_equal(encoding, "MP4A-LATM")) {
        return ZM_CODEC_AAC;
    }
    for (i = 0; i < sizeof(k_codecs) / sizeof(k_codecs[0]); ++i) {
        if (ci_equal(encoding, k_codecs[i].map.rtp_encoding)) {
            return k_codecs[i].id;
        }
    }
    return ZM_CODEC_INVALID;
}
