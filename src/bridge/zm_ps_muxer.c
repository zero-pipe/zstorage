/**
 * @file zm_ps_muxer.c
 * @brief MPEG-PS 打包桥：ZMS codec id → libmpeg ps_muxer。
 */
#include "bridge/zm_ps_muxer.h"

#include "mpeg-proto.h"
#include "mpeg-ps.h"

#include <stdlib.h>
#include <string.h>

struct zms_zm_ps_muxer {
    struct ps_muxer_t *ps;
    zms_zm_ps_write_fn write;
    void *user;
};

static void *ps_alloc(void *param, size_t bytes)
{
    (void)param;
    return malloc(bytes);
}

static void ps_free(void *param, void *packet)
{
    (void)param;
    free(packet);
}

static int ps_write(void *param, int stream, void *packet, size_t bytes)
{
    zms_zm_ps_muxer *m = (zms_zm_ps_muxer *)param;

    if (!m || !m->write || !packet || bytes == 0) {
        return -1;
    }
    return m->write(m->user, stream, (const uint8_t *)packet, bytes);
}

zms_zm_ps_muxer *zms_zm_ps_muxer_create(const zms_zm_ps_muxer_opts *opts)
{
    zms_zm_ps_muxer *m;
    struct ps_muxer_func_t func;

    if (!opts || !opts->write) {
        return NULL;
    }
    m = (zms_zm_ps_muxer *)calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    func.alloc = ps_alloc;
    func.free = ps_free;
    func.write = ps_write;
    m->ps = ps_muxer_create(&func, m);
    if (!m->ps) {
        free(m);
        return NULL;
    }
    m->write = opts->write;
    m->user = opts->user;
    return m;
}

void zms_zm_ps_muxer_destroy(zms_zm_ps_muxer *m)
{
    if (!m) {
        return;
    }
    if (m->ps) {
        ps_muxer_destroy(m->ps);
    }
    free(m);
}

static int ps_codecid_video(zms_codec_id codec)
{
    switch (codec) {
    case ZMS_CODEC_H264:
        return PSI_STREAM_H264;
    case ZMS_CODEC_H265:
        return PSI_STREAM_H265;
    case ZMS_CODEC_H266:
        return PSI_STREAM_H266;
    default:
        return 0;
    }
}

static int ps_codecid_audio(zms_codec_id codec)
{
    switch (codec) {
    case ZMS_CODEC_AAC:
        return PSI_STREAM_AAC;
    case ZMS_CODEC_G711A:
        return PSI_STREAM_AUDIO_G711A;
    case ZMS_CODEC_G711U:
        return PSI_STREAM_AUDIO_G711U;
    default:
        return 0;
    }
}

int zms_zm_ps_muxer_add_video(zms_zm_ps_muxer *m, zms_codec_id codec)
{
    int cid;

    if (!m || !m->ps) {
        return -1;
    }
    cid = ps_codecid_video(codec);
    if (!cid) {
        return -1;
    }
    return ps_muxer_add_stream(m->ps, cid, NULL, 0);
}

int zms_zm_ps_muxer_add_audio(zms_zm_ps_muxer *m, zms_codec_id codec)
{
    int cid;

    if (!m || !m->ps) {
        return -1;
    }
    cid = ps_codecid_audio(codec);
    if (!cid) {
        return -1;
    }
    return ps_muxer_add_stream(m->ps, cid, NULL, 0);
}

int zms_zm_ps_muxer_input_video(zms_zm_ps_muxer *m, int stream_id, const uint8_t *es,
                                 size_t len, int keyframe, int64_t pts_90k, int64_t dts_90k)
{
    int flags = keyframe ? MPEG_FLAG_IDR_FRAME : 0;

    if (!m || !m->ps || !es || len == 0) {
        return -1;
    }
    return ps_muxer_input(m->ps, stream_id, flags, pts_90k, dts_90k, es, len);
}

int zms_zm_ps_muxer_input_audio(zms_zm_ps_muxer *m, int stream_id, const uint8_t *es,
                                 size_t len, int64_t pts_90k)
{
    if (!m || !m->ps || !es || len == 0) {
        return -1;
    }
    return ps_muxer_input(m->ps, stream_id, 0, pts_90k, pts_90k, es, len);
}
