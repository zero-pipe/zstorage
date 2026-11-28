/**
 * @file play_binding.c
 * @brief 播放侧 reader 记账与 lane/egress_source 统一拆除。
 */
#include "zcs/play/play_binding.h"
#include "zcs/play/dispatch.h"
#include "zcs/core/event.h"
#include "zcs/vod/play/play_lane.h"
#include "zn/platform.h"

void zcs_play_binding_reader_start(zcs_play_binding *b, zcs_media_source *src, const char *player)
{
    if (!b || !src || !b->reader_attached) {
        return;
    }
    if (*b->reader_attached) {
        return;
    }
    if (b->source) {
        *b->source = src;
    }
    zcs_media_source_reader_add(src);
    *b->reader_attached = 1;
    if (b->play_start_ms) {
        *b->play_start_ms = zn_monotonic_ms();
    }
    if (player) {
        b->player = player;
    }
    zcs_media_event_play(src, b->player ? b->player : "play");
}

void zcs_play_binding_reader_stop(zcs_play_binding *b)
{
    zcs_media_source *src;
    const char *player;
    uint64_t start_ms;

    if (!b || !b->reader_attached || !*b->reader_attached) {
        return;
    }
    src = (b->source && *b->source) ? *b->source : NULL;
    player = b->player ? b->player : "play";
    start_ms = b->play_start_ms ? *b->play_start_ms : 0;
    if (src) {
        zcs_media_source_reader_remove(src);
        zcs_media_event_stop(src, player, start_ms);
    }
    *b->reader_attached = 0;
    if (b->play_start_ms) {
        *b->play_start_ms = 0;
    }
}

void zcs_play_binding_close_readers(zcs_play_binding *b)
{
    if (!b) {
        return;
    }
    /*
     * close 前先置空别名：VOD lane 可能持有曾挂到 play.readers.vod 的 buffer reader
     *（RTMP/RTSP）。拆除时勿释放该 reader。
     */
    if (b->gop_reader) {
        *b->gop_reader = NULL;
    }
    if (b->vod_reader) {
        *b->vod_reader = NULL;
    }
    if (b->play) {
        b->play->readers.gop = NULL;
        b->play->readers.vod = NULL;
        zcs_session_play_close(b->play);
    }
    if (b->vod_lane && *b->vod_lane) {
        zcs_vod_play_lane_close(*b->vod_lane);
        *b->vod_lane = NULL;
    }
}

void zcs_play_binding_close(zcs_play_binding *b, int clear_source)
{
    if (!b) {
        return;
    }
    zcs_play_binding_close_readers(b);
    zcs_play_binding_reader_stop(b);
    if (clear_source && b->source) {
        *b->source = NULL;
    }
}

void zcs_play_binding_demux_fill(zcs_play_binding *b, int max_pumps)
{
    if (!b || !b->vod_lane || !*b->vod_lane || max_pumps <= 0) {
        return;
    }
    zcs_vod_play_lane_demux_fill(*b->vod_lane, max_pumps);
}
