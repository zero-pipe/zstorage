#ifndef ZCS_CORE_STREAM_VOD_SOURCE_PORT_H
#define ZCS_CORE_STREAM_VOD_SOURCE_PORT_H

/**
 * @file vod_source_port.h
 * @brief 核心层对点播源的最小依赖。
 *
 * core 不得包含 service 头。本文件声明 stream hub/event/url 需要的点播能力，
 * 实现仍在 vod/io/source.c；链接后由同一归档提供。
 */
#include "zcs/core/stream/hub.h"
#include "zcs/zcs_export.h"
#include "zn/poller/poller.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 停止点播源的读取器并注销该点播源；非点播源或 NULL 时无操作。 */
ZCS_API void zcs_vod_source_stop(zcs_media_source *src);
/**
 * 查找或按需打开点播源（默认 vhost）。
 * 先查已注册源及 canonical 名；未命中且 app==record_app 时按点播文件打开。
 * @param poller 打开用的 poller（NULL 时回退 media events poller）
 * @return 可播放的源（可能本次新建）；未命中返回 NULL
 */
ZCS_API zcs_media_source *zcs_vod_source_find_or_open(const char *schema, const char *app,
                                                      const char *stream, zn_poller *poller);
/** 同 zcs_vod_source_find_or_open，但显式指定 vhost。 */
ZCS_API zcs_media_source *zcs_vod_source_find_or_open_ex(const char *schema, const char *vhost,
                                                         const char *app, const char *stream,
                                                         zn_poller *poller);

/**
 * 归一化点播 stream 名：去 rtmp 类型前缀、去 flv 包装后缀、必要时补默认扩展名。
 * @param stream_out 输出缓冲（需调用方提供）
 */
ZCS_API void zcs_vod_canonical_stream(const char *stream_in, char *stream_out, size_t out_cap);
/** 判断 stream 是否为点播 FLV 包装后缀（如 ".flv" 包装语义）。 */
ZCS_API int zcs_vod_stream_is_vod_flv_wrap_suffix(const char *stream);
/** 判断 stream 是否为原生 FLV 文件（非包装后缀且受支持）。 */
ZCS_API int zcs_vod_stream_is_native_flv_file(const char *stream);
/** 判断 stream 是否带磁盘容器扩展名且非原生文件类型（需经解封装）。 */
ZCS_API int zcs_vod_stream_has_disk_container_ext(const char *stream);
/**
 * 将 mp4 stream 名换成对应 m3u8 相对路径（把尾部 .mp4 改为 .m3u8）。
 * @return 1 成功；输入非法/非 .mp4/空间不足返回 0
 */
ZCS_API int zcs_vod_mp4_stream_to_m3u8_rel(const char *mp4_stream, char *m3u8_rel, size_t rel_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_STREAM_VOD_SOURCE_PORT_H */
