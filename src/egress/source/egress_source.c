/**
 * @file egress_source.c
 * @brief 为出站挂接直播 GOP / 点播 play-buffer 读者。
 */
#include "zcs/core/stream/buffer.h"
#include "zcs/core/stream/hub.h"
#include "zcs/egress/source/egress_source.h"
#include <string.h>

void zcs_egress_source_init(zcs_egress_source *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

void zcs_egress_source_close(zcs_egress_source *p)
{
    if (!p) {
        return;
    }
    zcs_media_subscriber_detach(&p->readers);
    memset(p, 0, sizeof(*p));
}

zn_err_t zcs_egress_source_open_live_gop(zcs_media_source *src, zcs_egress_source *p)
{
    if (!src || !p) {
        return ZN_ERR_INVALID;
    }
    zcs_egress_source_close(p);
    p->source = src;
    p->is_live = 1;
    return zcs_media_source_subscribe_gop(src, &p->readers);
}

zn_err_t zcs_egress_source_open_live_key(zcs_media_source *src, zcs_egress_source *p)
{
    if (!src || !p) {
        return ZN_ERR_INVALID;
    }
    zcs_egress_source_close(p);
    p->source = src;
    p->is_live = 1;
    if (zcs_media_source_subscribe(src, 0, &p->readers) != ZN_OK) {
        return ZN_ERR_STATE;
    }
    if (p->readers.gop) {
        zcs_gop_reader_seek_live_idr(p->readers.gop);
    }
    return ZN_OK;
}

zn_err_t zcs_egress_source_open_live(zcs_media_source *src, int seek_live, zcs_egress_source *p)
{
    if (!src || !p) {
        return ZN_ERR_INVALID;
    }
    zcs_egress_source_close(p);
    p->source = src;
    p->is_live = 1;
    return zcs_media_source_subscribe(src, seek_live, &p->readers);
}

zn_err_t zcs_egress_source_open_vod(zcs_media_source *src, zcs_egress_source *p)
{
    if (!src || !p) {
        return ZN_ERR_INVALID;
    }
    zcs_egress_source_close(p);
    p->source = src;
    p->is_live = 0;
    return zcs_media_source_subscribe_vod(src, &p->readers);
}

int zcs_egress_source_read_muxed(zcs_egress_source *p, zcs_gop_slot *slot, int timeout_ms)
{
    if (!p || !slot) {
        return -1;
    }
    if (p->readers.gop) {
        return zcs_gop_reader_read_muxed(p->readers.gop, slot, timeout_ms);
    }
    if (p->readers.vod) {
        return zcs_vod_buffer_reader_read_muxed(p->readers.vod, slot);
    }
    (void)timeout_ms;
    return 0;
}
