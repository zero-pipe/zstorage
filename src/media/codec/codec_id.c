/**
 * @file codec_id.c
 * @brief codec id 与名称 / track 类型映射。
 */
#include "zcs/media/codec/codec_id.h"
#include <string.h>
#if defined(_WIN32)
#define strcasecmp _stricmp
#endif

/* MPEG-TS stream_type id（镜像 libmpeg mpeg-proto.h；codec 元数据单一来源）。 */
#define PSI_H264 0x1b
#define PSI_H265 0x24
#define PSI_H266 0x33
#define PSI_AAC 0x0f
#define PSI_OPUS 0x9c
#define PSI_G711A 0x90
#define PSI_G711U 0x91
#define PSI_VP8 0x9d
#define PSI_VP9 0x9e
#define PSI_AV1 0x9f
/* 映射到基础 codec 的 stream_type 变体（见 libmpeg mpeg-proto.h）。 */
#define PSI_H265_SUBSET 0x25
#define PSI_H266_SUBSET 0x34
#define PSI_AAC_LATM 0x11
#define PSI_AAC_MPEG4 0x1c

static const zcs_codec_meta k_table[] = {
    {ZCS_CODEC_H264, ZCS_TRACK_VIDEO, "H264", "H264", PSI_H264},
    {ZCS_CODEC_H265, ZCS_TRACK_VIDEO, "H265", "H265", PSI_H265},
    {ZCS_CODEC_AAC, ZCS_TRACK_AUDIO, "AAC", "mpeg4-generic", PSI_AAC},
    {ZCS_CODEC_OPUS, ZCS_TRACK_AUDIO, "Opus", "opus", PSI_OPUS},
    {ZCS_CODEC_G711A, ZCS_TRACK_AUDIO, "G711A", "PCMA", PSI_G711A},
    {ZCS_CODEC_G711U, ZCS_TRACK_AUDIO, "G711U", "PCMU", PSI_G711U},
    {ZCS_CODEC_AV1, ZCS_TRACK_VIDEO, "AV1", "AV1", PSI_AV1},
    {ZCS_CODEC_VP8, ZCS_TRACK_VIDEO, "VP8", "VP8", PSI_VP8},
    {ZCS_CODEC_VP9, ZCS_TRACK_VIDEO, "VP9", "VP9", PSI_VP9},
    {ZCS_CODEC_H266, ZCS_TRACK_VIDEO, "H266", "H266", PSI_H266},
};

const zcs_codec_meta *zcs_codec_meta_get(zcs_codec_id id)
{
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); ++i) {
        if (k_table[i].id == id) {
            return &k_table[i];
        }
    }
    return NULL;
}

zcs_codec_id zcs_codec_id_from_name(const char *name)
{
    if (!name || !name[0]) {
        return ZCS_CODEC_INVALID;
    }
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); ++i) {
        if (strcasecmp(name, k_table[i].name) == 0 || strcasecmp(name, k_table[i].sdp_mime) == 0) {
            return k_table[i].id;
        }
    }
    if (strstr(name, "711") || strstr(name, "g711")) {
        if (strchr(name, 'u') || strchr(name, 'U') || strstr(name, "mulaw") ||
            strstr(name, "mu-law")) {
            return ZCS_CODEC_G711U;
        }
        return ZCS_CODEC_G711A;
    }
    return ZCS_CODEC_INVALID;
}

const char *zcs_codec_name(zcs_codec_id id)
{
    const zcs_codec_meta *m = zcs_codec_meta_get(id);
    return m ? m->name : "invalid";
}

zcs_track_type zcs_codec_track_type(zcs_codec_id id)
{
    const zcs_codec_meta *m = zcs_codec_meta_get(id);
    return m ? m->track : ZCS_TRACK_INVALID;
}

int zcs_codec_mpeg_psi(zcs_codec_id id)
{
    const zcs_codec_meta *m = zcs_codec_meta_get(id);
    return m ? m->mpeg_psi_id : 0;
}

zcs_codec_id zcs_codec_from_mpeg_psi(int psi)
{
    for (size_t i = 0; i < sizeof(k_table) / sizeof(k_table[0]); ++i) {
        if (k_table[i].mpeg_psi_id == psi) {
            return k_table[i].id;
        }
    }
    switch (psi) {
    case PSI_H265_SUBSET:
        return ZCS_CODEC_H265;
    case PSI_H266_SUBSET:
        return ZCS_CODEC_H266;
    case PSI_AAC_LATM:
    case PSI_AAC_MPEG4:
        return ZCS_CODEC_AAC;
    default:
        return ZCS_CODEC_INVALID;
    }
}

unsigned zcs_codec_count(void)
{
    return (unsigned)(sizeof(k_table) / sizeof(k_table[0]));
}
