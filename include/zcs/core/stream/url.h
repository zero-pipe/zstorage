/**
 * @file url.h
 * @brief 媒体 URL 构造与解析。
 */
#ifndef ZCS_CORE_STREAM_URL_H
#define ZCS_CORE_STREAM_URL_H

#include "zcs/core/stream/hub.h"
#include "zcs/zcs_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_MEDIA_URL_MAX 256

/** 推流/播放 URL（生产环境 stream 名通常带随机后缀，通过 API 查询） */
typedef struct zcs_media_urls {
    char origin[ZCS_MEDIA_URL_MAX];
    char rtmp_play[ZCS_MEDIA_URL_MAX];
    char rtsp_play[ZCS_MEDIA_URL_MAX];
    char http_flv[ZCS_MEDIA_URL_MAX];
    char http_ts[ZCS_MEDIA_URL_MAX];
    char http_mp4[ZCS_MEDIA_URL_MAX];
    char hls[ZCS_MEDIA_URL_MAX];
    char dash[ZCS_MEDIA_URL_MAX];
} zcs_media_urls;

/**
 * 填充某媒体源各协议的播放 URL。先清空 out；src/ports 为空时保留全空串。
 * 直播按协议能力逐项填充，点播走 canonical stream 规则；out->origin 为推流源地址。
 */
ZCS_API void zcs_media_urls_build(zcs_media_urls *out, const zcs_media_source *src,
                                  const zcs_media_server_ports *ports);

/** 推流注册后打印完整推拉地址（写入日志） */
ZCS_API void zcs_media_urls_log_publish(const zcs_media_source *src,
                                        const zcs_media_server_ports *ports);

/** 单路 URL 明细（registry 周期 dump 用） */
ZCS_API void zcs_media_urls_log_stream(const zcs_media_source *src,
                                       const zcs_media_server_ports *ports);

/** 去掉 HTTP 拉流路径上的 *.{mp4|mkv|mov|m4v}.flv / .flv / .m3u8 / .mpd / .mp4，得到协议无关的 stream */
ZCS_API void zcs_media_path_strip_play_suffix(char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_STREAM_URL_H */
