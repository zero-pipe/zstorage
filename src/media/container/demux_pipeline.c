/**
 * @file demux_pipeline.c
 * @brief 解复用管线（FLV tag 输入 → packet）。
 */
#include "zms/media/container/demux_pipeline.h"
#include "zms/media/container/flv/flv_tag_demuxer.h"
#include "zms/media/container/mpegts/mpegts_demuxer.h"
#include "zms/media/media_limits.h"
#include <stdlib.h>
#include <string.h>

struct zms_demux_pipeline {
    zms_container_demux_opts container_cfg;
    void *container;
    const zms_container_demuxer_ops *container_ops;
    zms_payload_frame_cb on_frame;
    zms_demux_pipeline_h264_ps_fn on_mpegts_h264_ps;
    void *user;
    zms_flv_tag_demuxer *flv;
    zms_mpegts_demuxer *mpegts;
};

static void pipeline_on_frame(const zms_frame *frame, void *user)
{
    zms_demux_pipeline *p = (zms_demux_pipeline *)user;
    if (p && p->on_frame) {
        p->on_frame(frame, p->user);
    }
}

static void pipeline_on_h264_ps(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                                size_t pps_len, void *user)
{
    zms_demux_pipeline *p = (zms_demux_pipeline *)user;
    if (p && p->on_mpegts_h264_ps) {
        p->on_mpegts_h264_ps(sps, sps_len, pps, pps_len, p->user);
    }
}

static void pipeline_on_container(const zms_container_packet *pkt, void *user)
{
    zms_demux_pipeline *p = (zms_demux_pipeline *)user;
    int i;
    if (!p || !pkt) {
        return;
    }

    if (pkt->kind == ZMS_CONTAINER_PKT_FLV_TAG && p->flv) {
        (void)zms_flv_tag_demuxer_input(p->flv, pkt->flv_type_id, pkt->data, pkt->len,
                                        pkt->tag_dts_ms);
    }
}

zms_demux_pipeline *zms_demux_pipeline_create(const zms_demux_pipeline_opts *opts)
{
    zms_demux_pipeline *p;
    zms_flv_tag_demuxer_opts fopts;
    if (!opts || !opts->on_frame) {
        return NULL;
    }
    if (!zms_container_demuxer_find(opts->container)) {
        return NULL;
    }

    p = (zms_demux_pipeline *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->on_frame = opts->on_frame;
    p->on_mpegts_h264_ps = opts->on_mpegts_h264_ps;
    p->user = opts->user;
    p->container_cfg.id = opts->container;
    p->container_cfg.on_packet = pipeline_on_container;
    p->container_cfg.user = p;
    p->container_ops = zms_container_demuxer_find(opts->container);
    if (!p->container_ops || !p->container_ops->create) {
        free(p);
        return NULL;
    }
    p->container = p->container_ops->create(&p->container_cfg);
    if (!p->container) {
        free(p);
        return NULL;
    }

    if (opts->container == ZMS_CONTAINER_FLV_TAG) {
        memset(&fopts, 0, sizeof(fopts));
        fopts.on_frame = pipeline_on_frame;
        fopts.user = p;
        p->flv = zms_flv_tag_demuxer_create(&fopts);
        if (!p->flv) {
            p->container_ops->destroy(p->container);
            free(p);
            return NULL;
        }
    } else if (opts->container == ZMS_CONTAINER_MPEGTS) {
        zms_mpegts_demuxer_opts mopts;

        memset(&mopts, 0, sizeof(mopts));
        mopts.on_frame = pipeline_on_frame;
        mopts.on_h264_ps = pipeline_on_h264_ps;
        mopts.user = p;
        p->mpegts = zms_mpegts_demuxer_create(&mopts);
        if (!p->mpegts) {
            p->container_ops->destroy(p->container);
            free(p);
            return NULL;
        }
    }
    return p;
}

zn_err_t zms_demux_pipeline_feed(zms_demux_pipeline *p, const uint8_t *buf, size_t len)
{
    if (!p || !buf || len == 0) {
        return ZN_ERR_INVALID;
    }
    if (p->mpegts) {
        return zms_mpegts_demuxer_feed(p->mpegts, buf, len);
    }
    if (!p->container_ops || !p->container_ops->feed) {
        return ZN_ERR_INVALID;
    }
    return p->container_ops->feed(p->container, buf, len);
}

void zms_demux_pipeline_flush(zms_demux_pipeline *p)
{
    if (!p) {
        return;
    }
    if (p->mpegts) {
        (void)zms_mpegts_demuxer_flush(p->mpegts);
    }
}

void zms_demux_pipeline_destroy(zms_demux_pipeline *p)
{
    if (!p) {
        return;
    }
    if (p->flv) {
        zms_flv_tag_demuxer_destroy(p->flv);
    }
    if (p->mpegts) {
        zms_mpegts_demuxer_destroy(p->mpegts);
    }
    if (p->container && p->container_ops && p->container_ops->destroy) {
        p->container_ops->destroy(p->container);
    }
    free(p);
}


zn_err_t zms_demux_pipeline_input_flv_tag(zms_demux_pipeline *p, int track_index, uint8_t type_id,
                                           const uint8_t *body, size_t len, uint32_t tag_dts_ms)
{
    (void)track_index;
    if (!p || !p->container_ops || !p->container_ops->input_tag) {
        return ZN_ERR_INVALID;
    }
    return p->container_ops->input_tag(p->container, type_id, body, len, tag_dts_ms);
}
