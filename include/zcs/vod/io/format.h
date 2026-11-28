#ifndef ZCS_VOD_IO_FORMAT_H
#define ZCS_VOD_IO_FORMAT_H

/**
 * @file format.h
 * @brief 点播磁盘格式描述符注册表。
 *
 * VOD format 绑定文件扩展名、容器 demux id、FLV-wrap 支持与时长探测。
 * 新文件容器（如 AVI）应在 VOD 路径解析前在此注册描述符。
 */
#include "zcs/media/container/container_registry.h"
#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t (*zcs_vod_format_probe_duration_fn)(const char *path);

typedef struct zcs_vod_format_desc {
    const char *name;
    zcs_container_id container;
    const char *const *extensions;
    const char *default_extension;
    int can_flv_wrap;
    int native_file_send;
    zcs_vod_format_probe_duration_fn probe_duration_ms;
} zcs_vod_format_desc;

typedef int (*zcs_vod_format_visit_cb)(const zcs_vod_format_desc *fmt, const char *ext, void *user);

/**
 * 注册（或按 name 覆盖）一个 VOD 格式描述符。
 * 描述符指针被保存而非拷贝，故必须是静态/长生命周期数据；容量上限 16。
 */
ZCS_API void zcs_vod_format_register(const zcs_vod_format_desc *fmt);
/** 注册内置格式（mov-family/mkv/flv-file）；幂等，重复调用无副作用。 */
ZCS_API void zcs_vod_format_register_all(void);

/**
 * 按扩展名查找格式（大小写不敏感，ext 形如 ".mp4"）。
 * 查找前会惰性注册内置格式。
 * @return 命中的描述符（静态借用）；未找到返回 NULL。
 */
ZCS_API const zcs_vod_format_desc *zcs_vod_format_find_by_extension(const char *ext);
/** 从路径提取扩展名后按扩展名查找；忽略目录中的点号。 */
ZCS_API const zcs_vod_format_desc *zcs_vod_format_find_by_path(const char *path);
/**
 * 遍历所有注册格式的每个扩展名。
 * @param cb   回调；返回非 0 提前终止
 * @param user 透传上下文
 * @return 已遍历的扩展名条数。
 */
ZCS_API int zcs_vod_format_foreach_extension(zcs_vod_format_visit_cb cb, void *user);

/** 默认 VOD 扩展名（首个带 default_extension 的格式；兜底 ".mp4"）。 */
ZCS_API const char *zcs_vod_format_default_extension(void);
/** stream 是否带已注册磁盘容器后缀（按扩展名判断）。 */
ZCS_API int zcs_vod_format_stream_has_supported_ext(const char *stream);
/** stream 是否为可原生直出格式（format->native_file_send 置位，如 .flv 文件）。 */
ZCS_API int zcs_vod_format_stream_is_native_file(const char *stream);
/** `*.{format}.flv` 封装后缀判定：磁盘扩展名对应格式支持 FLV-wrap 时返回 1。 */
ZCS_API int zcs_vod_format_stream_is_flv_wrap_suffix(const char *stream);
/**
 * 按路径所属格式探测时长（ms）。
 * @return 时长；格式不支持探测或失败返回 0。
 */
ZCS_API uint64_t zcs_vod_format_probe_duration_ms(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_IO_FORMAT_H */
