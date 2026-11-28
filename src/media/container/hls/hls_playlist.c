/**
 * @file hls_playlist.c
 * @brief M3U8 播放列表接口实现（封装 zm libhls hls_m3u8）。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/media/container/hls/hls_playlist.h"
#include "zm/format/hls.h"

zcs_hls_m3u8 *zcs_hls_m3u8_create(int live, int version)
{
    return (zcs_hls_m3u8 *)hls_m3u8_create(live, version);
}

void zcs_hls_m3u8_destroy(zcs_hls_m3u8 *m)
{
    hls_m3u8_destroy((hls_m3u8_t *)m);
}

int zcs_hls_m3u8_set_x_map(zcs_hls_m3u8 *m, const char *name)
{
    return hls_m3u8_set_x_map((hls_m3u8_t *)m, name);
}

int zcs_hls_m3u8_add(zcs_hls_m3u8 *m, const char *name, int64_t pts_ms, int64_t duration_ms,
                     int discontinuity)
{
    return hls_m3u8_add((hls_m3u8_t *)m, name, pts_ms, duration_ms, discontinuity);
}

int zcs_hls_m3u8_playlist(zcs_hls_m3u8 *m, int eof, char *out, size_t cap)
{
    return hls_m3u8_playlist((hls_m3u8_t *)m, eof, out, cap);
}
