#ifndef ZCS_PLAY_PLAY_BINDING_H
#define ZCS_PLAY_PLAY_BINDING_H

/**
 * @file play_binding.h
 * @brief 播放侧窄接口：reader 记账 + egress_source/lane 拆除。
 *
 * 不拥有协议线路 muxer（FLV/TS/RTP）。会话保留后者并调用本模块处理
 * 重复的 close / reader_add|remove / media_event_stop 路径。
 *
 * Binding 为视图：字段指向会话结构体内的成员。
 */
#include "zcs/egress/source/egress_source.h"
#include "zcs/zcs_export.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_vod_play_lane zcs_vod_play_lane;
typedef struct zcs_vod_buffer_reader zcs_vod_buffer_reader;

typedef struct zcs_play_binding {
    zcs_media_source **source;
    zcs_egress_source *play;
    zcs_gop_reader **gop_reader;
    zcs_vod_buffer_reader **vod_reader;
    zcs_vod_play_lane **vod_lane;
    int *reader_attached;
    uint64_t *play_start_ms;
    /** media_event_stop 的播放器 schema（如 "rtmp"、"rtsp"、"srt"、"http-flv"）。 */
    const char *player;
} zcs_play_binding;

/** 标记播放 reader 已附着并触发 media_event_play。已附着时幂等。 */
ZCS_API void zcs_play_binding_reader_start(zcs_play_binding *b, zcs_media_source *src,
                                           const char *player);

/**
 * reader_attached 时执行 reader_remove + media_event_stop。
 * 清除 *reader_attached 与 *play_start_ms；不清除 *source。
 */
ZCS_API void zcs_play_binding_reader_stop(zcs_play_binding *b);

/**
 * 关闭 egress_source 读者、清除缓存的 gop/vod reader 指针、关闭 vod_lane。
 * 不解除 session_dispatcher，不触碰 reader_attached。
 */
ZCS_API void zcs_play_binding_close_readers(zcs_play_binding *b);

/**
 * close_readers + reader_stop。clear_source != 0 且 source 已设时，*source = NULL。
 */
ZCS_API void zcs_play_binding_close(zcs_play_binding *b, int clear_source);

/** 存在 lane 时预填充 VOD demux（否则 no-op）。 */
ZCS_API void zcs_play_binding_demux_fill(zcs_play_binding *b, int max_pumps);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_PLAY_BINDING_H */
