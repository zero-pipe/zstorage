/**
 * @file source.h
 * @brief VOD 源配置与打开。
 */
#ifndef ZMS_VOD_IO_SOURCE_H
#define ZMS_VOD_IO_SOURCE_H

/**
 * 文件点播源：已注册 VOD format demux → vod_buffer，供 RTSP/RTMP/HTTP-FLV（不复用 gop_queue）。
 */
#include "zms/core/stream/hub.h"
#include "zms/ops/config/config.h"
#include "zms/core/stream/buffer.h"
#include "zms/zms_export.h"
#include "zn/poller/poller.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_vod_config {
    char record_app[ZMS_APP_MAX];
    char record_root[ZMS_CFG_PATH_MAX];
    int file_repeat;
} zms_vod_config;

/** 以默认值填充 cfg：record_app="vod"、record_root="./www"、file_repeat=0。 */
ZMS_API void zms_vod_config_default(zms_vod_config *cfg);
/**
 * 用全局配置填充进程级 VOD 配置快照（读取 record.app/root/file_repeat）。
 * 注意：若 record.root 含 "/record" 则不覆盖 VOD root（那是直播录像专用目录）。
 */
ZMS_API void zms_vod_config_apply(zms_config *cfg);
/** 取进程级 VOD 配置（静态借用，勿修改、勿释放）。 */
ZMS_API const zms_vod_config *zms_vod_config_get(void);

/** app 是否为点播 record_app（配置项） */
ZMS_API int zms_vod_is_record_app(const char *app);

/** 相对路径安全校验（禁止 .. 与绝对路径） */
ZMS_API int zms_vod_rel_path_safe(const char *rel);

/** 解析磁盘路径：{root}/{app}/{rel}，URL 与目录一一对应 */
ZMS_API int zms_vod_resolve_rel_path(const char *app, const char *rel, char *out, size_t out_cap);

/**
 * 解析点播磁盘文件：{root}/{app}/{stream}
 * - `foo.{format}.flv`：format FLV-wrap 时，打开对应磁盘容器文件
 * - `foo.flv` / `foo.mkv` / `foo.mov` 等：已注册磁盘格式文件
 */
ZMS_API int zms_vod_resolve_file_path(const char *app, const char *stream, char *out,
                                      size_t out_cap);

/** `*.{format}.flv`：容器点播转 FLV 拉流名，不是磁盘上的 `.flv` 文件 */
ZMS_API int zms_vod_stream_is_vod_flv_wrap_suffix(const char *stream);

/** 磁盘原生文件直出格式（当前为 `.flv`，排除 `*.{format}.flv` 封装后缀） */
ZMS_API int zms_vod_stream_is_native_flv_file(const char *stream);

/** stream 是否已带已注册磁盘容器后缀（非原生直出文件） */
ZMS_API int zms_vod_stream_has_disk_container_ext(const char *stream);

/**
 * 点播 stream 规范名：RTMP 前缀；`*.{format}.flv` 去尾 `.flv`。
 * 原生容器后缀保留；无后缀则补默认 VOD format 后缀。
 */
ZMS_API void zms_vod_canonical_stream(const char *stream_in, char *stream_out, size_t out_cap);

/** m3u8 相对路径转对应 MP4 stream（foo/bar.m3u8 → foo/bar.mp4） */
ZMS_API int zms_vod_m3u8_rel_to_mp4_stream(const char *m3u8_rel, char *stream_out, size_t out_cap);

/** MP4 stream 转对应 m3u8 相对路径（foo/bar.mp4 → foo/bar.m3u8） */
ZMS_API int zms_vod_mp4_stream_to_m3u8_rel(const char *mp4_stream, char *m3u8_rel, size_t rel_cap);

/** 去掉 .mp4 得到基名，如 test.mp4 → test */
ZMS_API void zms_vod_stream_basename(const char *stream_in, char *base_out, size_t out_cap);

/** 由 TS 相对路径推断 MP4 stream（按需动态生成分片） */
ZMS_API int zms_vod_infer_stream_from_ts(const char *app, const char *ts_rel, char *stream_out,
                                         size_t out_cap);

/**
 * 打开文件点播并注册媒体源（publish_origin=mp4_vod，历史枚举名）。
 * @param file_path 空则按 app/stream 自动解析
 */
ZMS_API zms_media_source *zms_vod_source_open(const char *app, const char *stream,
                                              const char *file_path, zn_poller *poller);
/**
 * 打开文件点播并注册媒体源（带 vhost，RTMP schema）。
 * stream 会先被规范化为规范名；file_path 为空时按 app/stream 自动解析磁盘路径。
 * 成功后立即绑定 poller 并发出 MP4_VOD publish 事件。
 * @return 注册的源（由 VOD 生命周期持有）；失败返回 NULL。
 */
ZMS_API zms_media_source *zms_vod_source_open_vhost(const char *vhost, const char *app,
                                                    const char *stream, const char *file_path,
                                                    zn_poller *poller);
/**
 * 停止点播缓存并注销：关闭 reader、发 publish_fini 事件、unregister_vod。
 * 仅对 is_vod 源有效；调用后 src 被释放，不得再用。
 */
ZMS_API void zms_vod_source_stop(zms_media_source *src);

/** find_for_play 懒加载：app==record_app 时按路径打开 */
ZMS_API zms_media_source *zms_vod_source_find_or_open(const char *schema, const char *app,
                                                      const char *stream, zn_poller *poller);
/**
 * find_for_play 懒加载（带 vhost）。
 * 先按原名与规范名查已注册源；仅当 app==record_app 时才按路径按需打开。
 * poller 为 NULL 时回退到全局事件 poller。
 * @return 命中的源（借用）；未命中/无法打开返回 NULL。
 */
ZMS_API zms_media_source *zms_vod_source_find_or_open_ex(const char *schema, const char *vhost,
                                                         const char *app, const char *stream,
                                                         zn_poller *poller);

/** PLAY/seek：重建 demux+fifo 到文件头同步 */
ZMS_API void zms_vod_source_prepare_play(zms_media_source *src);
/** 按媒体时间 seek(ms)，返回实际位置 */
ZMS_API uint64_t zms_vod_source_seek_ms(zms_media_source *src, uint64_t ms);
/** 单读者 seek：优先 fifo 内重定位，否则 demux（共享 fifo） */
ZMS_API uint64_t zms_vod_source_seek_for_reader(zms_media_source *src, zms_vod_buffer_reader *rd,
                                                uint64_t ms);
/** 已打开点播源的总时长（ms）；无变量源或未知返回 0。 */
ZMS_API uint64_t zms_vod_source_duration_ms(const zms_media_source *src);
/**
 * 已打开点播源的磁盘路径（loadMP4File 显式路径或解析路径）。
 * @return 1 成功写入 out，0 无 publisher / 无路径
 */
ZMS_API int zms_vod_source_file_path(const zms_media_source *src, char *out, size_t out_cap);
/** 只读探测 VOD 文件 duration，不注册媒体源、不启 HLS */
ZMS_API uint64_t zms_vod_probe_duration_ms(const char *app, const char *stream);
/** 取点播源的 FLV 文件索引（仅 FLV 容器）；无则返回 NULL，指针借用。 */
ZMS_API const struct zms_vod_flv_index *zms_vod_source_flv_index(const zms_media_source *src);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_VOD_IO_SOURCE_H */
