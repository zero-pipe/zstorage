/**
 * @file zm_rtp_ingest.c
 * @brief RTP → zm reader 的入站桥（track/输入/销毁）。
 */
#include "bridge/zm_rtp_ingest.h"

#include "bridge/zm_zcs_bridge.h"
#include "zm/transport/rtp.h"
#include "zn/util/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct zcs_zm_rtp_track {
    zm_reader *reader;
    zcs_codec_id codec;
    int payload_type;
    int clock_hz;
} zcs_zm_rtp_track;

struct zcs_zm_rtp_ingest {
    zcs_zm_rtp_track *tracks;
    int max_tracks;
    int jitter_ms;
    zcs_zm_rtp_packet_sink sink;
    void *user;
};

zcs_zm_rtp_ingest *zcs_zm_rtp_ingest_create(int max_tracks, int jitter_ms,
                                               zcs_zm_rtp_packet_sink sink, void *user)
{
    zcs_zm_rtp_ingest *z;

    if (max_tracks <= 0 || !sink) {
        return NULL;
    }
    z = (zcs_zm_rtp_ingest *)calloc(1, sizeof(*z));
    if (!z) {
        return NULL;
    }
    z->tracks = (zcs_zm_rtp_track *)calloc((size_t)max_tracks, sizeof(*z->tracks));
    if (!z->tracks) {
        free(z);
        return NULL;
    }
    z->max_tracks = max_tracks;
    z->jitter_ms = jitter_ms;
    z->sink = sink;
    z->user = user;
    return z;
}

int zcs_zm_rtp_ingest_track(zcs_zm_rtp_ingest *z, int track_idx, zcs_codec_id codec,
                             int payload_type, int clock_hz, const char *encoding)
{
    zcs_zm_rtp_track *t;
    zm_rtp_reader_cfg cfg;

    if (!z || track_idx < 0 || track_idx >= z->max_tracks) {
        return -1;
    }
    t = &z->tracks[track_idx];
    if (t->reader) {
        return 0;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.codec = zcs_zm_codec_from_zcs(codec);
    if (cfg.codec == ZM_CODEC_INVALID) {
        return -1;
    }
    cfg.payload_type = payload_type;
    cfg.clock_hz = clock_hz;
    cfg.encoding = encoding;
    cfg.jitter_ms = z->jitter_ms;
    t->reader = zm_rtp_reader_open(&cfg);
    if (!t->reader) {
        return -1;
    }
    t->codec = codec;
    t->payload_type = payload_type;
    t->clock_hz = clock_hz;
    zn_info("zm rtp track=%d codec=%d pt=%d clock=%d", track_idx, (int)codec, payload_type,
             clock_hz);
    return 0;
}

int zcs_zm_rtp_ingest_input(zcs_zm_rtp_ingest *z, int track_idx, const void *rtp, size_t len)
{
    zcs_zm_rtp_track *t;
    zm_packet pkt;
    int rc;

    if (!z || !rtp || len == 0 || track_idx < 0 || track_idx >= z->max_tracks) {
        return -1;
    }
    t = &z->tracks[track_idx];
    if (!t->reader) {
        return -1;
    }
    if (zm_rtp_reader_input_rtp(t->reader, rtp, len) != ZM_OK) {
        return -1;
    }
    while ((rc = zm_reader_read(t->reader, &pkt)) == 1) {
        z->sink(z->user, &pkt);
        zm_packet_clear(&pkt);
    }
    return rc < 0 ? -1 : 0;
}

void zcs_zm_rtp_ingest_destroy(zcs_zm_rtp_ingest *z)
{
    int i;

    if (!z) {
        return;
    }
    for (i = 0; i < z->max_tracks; ++i) {
        if (z->tracks[i].reader) {
            zm_reader_close(z->tracks[i].reader);
        }
    }
    free(z->tracks);
    free(z);
}
