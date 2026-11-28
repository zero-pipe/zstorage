/**
 * @file flv_tag_container_demux.c
 * @brief FLV tag → 容器解复用适配。
 */
#include "zcs/media/container/container_registry.h"
#include "zcs/media/container/flv/flv_tag_framer.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    zcs_container_demux_opts cfg;
    zcs_flv_tag_framer *flv;
} flv_tag_ctx;

static void flv_tag_on_byte(uint8_t type, const uint8_t *body, size_t len, uint32_t tag_dts_ms,
                            void *user)
{
    flv_tag_ctx *c = (flv_tag_ctx *)user;
    zcs_container_packet pkt;

    if (!c || !body || len == 0) {
        return;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZCS_CONTAINER_PKT_FLV_TAG;
    pkt.flv_type_id = type;
    pkt.tag_dts_ms = tag_dts_ms;
    pkt.data = body;
    pkt.len = len;
    if (c->cfg.on_packet) {
        c->cfg.on_packet(&pkt, c->cfg.user);
    }
}

static void *flv_tag_create(const zcs_container_demux_opts *opts)
{
    flv_tag_ctx *c;

    if (!opts || !opts->on_packet) {
        return NULL;
    }
    c = (flv_tag_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->cfg = *opts;
    c->flv = zcs_flv_tag_framer_create();
    if (!c->flv) {
        free(c);
        return NULL;
    }
    return c;
}

static void flv_tag_destroy(void *ctx)
{
    flv_tag_ctx *c = (flv_tag_ctx *)ctx;
    if (!c) {
        return;
    }
    zcs_flv_tag_framer_destroy(c->flv);
    free(c);
}

static zn_err_t flv_tag_feed(void *ctx, const uint8_t *buf, size_t len)
{
    flv_tag_ctx *c = (flv_tag_ctx *)ctx;
    if (!c || !c->flv || !buf || len == 0) {
        return ZN_ERR_INVALID;
    }
    return zcs_flv_tag_framer_feed(c->flv, buf, len, flv_tag_on_byte, c);
}

static zn_err_t flv_tag_input_tag(void *ctx, uint8_t type_id, const uint8_t *body, size_t len,
                                   uint32_t tag_dts_ms)
{
    flv_tag_ctx *c = (flv_tag_ctx *)ctx;
    zcs_container_packet pkt;

    if (!c || !body || len == 0) {
        return ZN_ERR_INVALID;
    }
    memset(&pkt, 0, sizeof(pkt));
    pkt.kind = ZCS_CONTAINER_PKT_FLV_TAG;
    pkt.flv_type_id = type_id;
    pkt.tag_dts_ms = tag_dts_ms;
    pkt.data = body;
    pkt.len = len;
    if (c->cfg.on_packet) {
        c->cfg.on_packet(&pkt, c->cfg.user);
    }
    return ZN_OK;
}

const zcs_container_demuxer_ops zcs_container_flv_tag_ops = {
    ZCS_CONTAINER_FLV_TAG, "flv-tag",    flv_tag_create,
    flv_tag_destroy,       flv_tag_feed, flv_tag_input_tag,
};
