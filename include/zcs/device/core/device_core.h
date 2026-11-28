/**
 * @file device_core.h
 * @brief 设备/通道/能力注册表：ONVIF、GB28181 及后续协议共用的唯一设备模型。
 *
 * 设计约束：
 *   - 协议适配器只做线协议；设备/通道/能力从这里读写；
 *   - 通道以 (vhost, app, stream) 为媒体键，可选带 GB 20 位通道 ID；
 *   - 不持有 zcs_media_source 指针，在线状态由 sync 从 hub 刷新，避免悬垂；
 *   - foreach 回调运行在内部锁内，回调里不要再次调用本模块。
 */
#ifndef ZCS_DEVICE_CORE_DEVICE_CORE_H
#define ZCS_DEVICE_CORE_DEVICE_CORE_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_DEVICE_ID_MAX 64
#define ZCS_DEVICE_NAME_MAX 128
#define ZCS_DEVICE_VHOST_MAX 128
#define ZCS_DEVICE_APP_MAX 64
#define ZCS_DEVICE_STREAM_MAX 128
#define ZCS_DEVICE_CORE_MAX_CHANNELS 128

/** 能力位图 */
enum zcs_device_cap {
    ZCS_DEV_CAP_LIVE = 1u << 0,
    ZCS_DEV_CAP_PLAYBACK = 1u << 1,
    ZCS_DEV_CAP_PTZ = 1u << 2,
    ZCS_DEV_CAP_SNAPSHOT = 1u << 3,
    ZCS_DEV_CAP_EVENTS = 1u << 4,
    ZCS_DEV_CAP_RECORD_QUERY = 1u << 5,
};

/** 通道来源/可见性标志 */
enum zcs_device_channel_flag {
    /** 显式注册（例如 GB28181 配置的通道），sync 不会删除 */
    ZCS_DEV_CH_CONFIGURED = 1u << 0,
    /** 由 sync 从 hub 在线流发现的临时通道 */
    ZCS_DEV_CH_DISCOVERED = 1u << 1,
    /** 对 ONVIF 设备端可见 */
    ZCS_DEV_CH_ONVIF = 1u << 2,
    /** 对 GB28181 Catalog 可见 */
    ZCS_DEV_CH_GB = 1u << 3,
};

typedef struct zcs_device_channel_info {
    /** 协议无关通道标识（配置通道为 GB ID；发现通道为 app/stream） */
    char id[ZCS_DEVICE_ID_MAX];
    /** GB28181 20 位设备/通道编码；非 GB 通道为空 */
    char gb_id[ZCS_DEVICE_ID_MAX];
    char name[ZCS_DEVICE_NAME_MAX];
    char vhost[ZCS_DEVICE_VHOST_MAX];
    char app[ZCS_DEVICE_APP_MAX];
    char stream[ZCS_DEVICE_STREAM_MAX];
    char manufacturer[64];
    char model[64];
    unsigned flags; /**< ZCS_DEV_CH_* */
    unsigned caps;  /**< ZCS_DEV_CAP_* */
    int online;     /**< 最近一次 sync 时是否在推流 */
} zcs_device_channel_info;

typedef int (*zcs_device_channel_visit_fn)(const zcs_device_channel_info *ch, void *user);

#define ZCS_DEVICE_RECORD_MAX 128

/** 通用录像条目（GB RecordInfo / ONVIF Recording 共用） */
typedef struct zcs_device_record_entry {
    char channel_id[ZCS_DEVICE_ID_MAX];
    char name[128];
    /** 相对录像根目录的路径：<stream>/<date>/<file> */
    char file_path[512];
    /** YYYY-MM-DDTHH:MM:SS */
    char start_time[32];
    char end_time[32];
    char type[16];
} zcs_device_record_entry;

/**
 * 录像查询结果：不透明句柄。
 *
 * 结果集最大 128 条 × 784B（~98KB），做成值类型会让调用方不小心把它放上栈；
 * 一律 create/query 返回指针 + 只读迭代访问。
 */
typedef struct zcs_device_record_list zcs_device_record_list;

/** 进程生命周期内初始化/销毁（幂等）。 */
ZCS_API void zcs_device_core_init(void);
/** 清除所有通道并销毁注册表（幂等）；进程退出时调用。 */
ZCS_API void zcs_device_core_fini(void);

/**
 * 注册/更新一个通道（按 vhost/app/stream 作为键）。
 * flags/caps 会与已有值按位或合并；用于 GB 配置通道或 ONVIF 静态设备。
 */
ZCS_API int zcs_device_core_register(const zcs_device_channel_info *ch);

/** 按 id 或 gb_id 删除（仅删除 CONFIGURED 条目）。 */
ZCS_API int zcs_device_core_unregister(const char *id);

/**
 * 从 hub 同步在线流：
 *   - 已配置通道刷新 online；
 *   - 未配置的在线流生成 DISCOVERED|ONVIF 条目；
 *   - 已离线的 DISCOVERED 条目删除。
 * @return 同步后的通道总数。
 */
ZCS_API int zcs_device_core_sync_live(void);

/** 按 id / gb_id 查（拷贝出）。@return 0 成功，-1 不存在。 */
ZCS_API int zcs_device_core_get(const char *id, zcs_device_channel_info *out);

/** 按媒体键查（vhost 可空，按 [general] enableVhost 归一化）。 */
ZCS_API int zcs_device_core_get_key(const char *vhost, const char *app, const char *stream,
                                    zcs_device_channel_info *out);

/**
 * 遍历满足条件的通道（回调内不得再调用本模块）。
 * @param require_caps 0=不限；否则需包含全部能力位
 * @param require_flags 0=不限；否则需包含全部标志位
 */
ZCS_API int zcs_device_core_foreach(unsigned require_caps, unsigned require_flags,
                                    zcs_device_channel_visit_fn fn, void *user);

/** 统计满足条件的通道数。 */
ZCS_API size_t zcs_device_core_count(unsigned require_caps, unsigned require_flags);

/** 任一通道是否具备指定能力（能力门控用）。 */
ZCS_API int zcs_device_core_any_cap(unsigned caps);

/** 所有通道能力的并集（能力宣告 / 运维诊断）。 */
ZCS_API unsigned zcs_device_core_caps(void);

/** 设置录像根目录（进程级；通常来自 [record] root 或 GB28181.record_root）。 */
ZCS_API void zcs_device_core_set_record_root(const char *root);
/** @return 当前录像根目录（内部静态缓冲，借用，勿释放/长期持有）。 */
ZCS_API const char *zcs_device_core_record_root(void);

/**
 * 按通道 + 时间范围查询录像（文件名即起录时间，mtime 为结束时间）。
 * @param channel_id 通道 id 或 gb_id
 * @return 结果列表（调用方用 zcs_device_record_list_free 释放）；通道/根目录无效返回 NULL
 */
ZCS_API zcs_device_record_list *zcs_device_record_query(const char *channel_id,
                                                        const char *start_time,
                                                        const char *end_time);

/** 释放查询结果；NULL 安全。 */
ZCS_API void zcs_device_record_list_free(zcs_device_record_list *list);
/** 结果条数。 */
ZCS_API size_t zcs_device_record_list_count(const zcs_device_record_list *list);
/** 第 index 条（越界返回 NULL）；指针有效期到 list 释放为止。 */
ZCS_API const zcs_device_record_entry *zcs_device_record_list_at(const zcs_device_record_list *list,
                                                                 size_t index);

/**
 * 选取与时间范围相交的第一条录像，并给出相对文件起点的偏移。
 * @param offset_ms 可为 NULL
 * @return 0 找到，-1 没有
 */
ZCS_API int zcs_device_record_pick(const char *channel_id, const char *start_time,
                                   const char *end_time, zcs_device_record_entry *out,
                                   int64_t *offset_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_DEVICE_CORE_DEVICE_CORE_H */
