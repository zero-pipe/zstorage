/**
 * @file zs_slot.h
 * @brief 预分配槽位：固定大小的落盘单元，内含头 / 关键帧索引 / PS 数据区。
 *
 * 物理布局（单文件，创建时一次性预分配 slot_bytes）：
 *
 *   偏移 0                        Zone A 槽位头   512B（实际占前 4KB，其余保留）
 *   ZONE_A_BYTES .. +index_bytes  Zone B 关键帧索引（每码 32 字节）
 *   +index_bytes .. file_bytes    Zone C PS 数据区（顺序追加完整 PS pack）
 *
 * 设计要点：
 *   - Zone C 里是**标准 MPEG-PS 字节流**，从任意关键帧偏移开始都能被标准解析器
 *     直接解（GB28181/第三方拉文件不需要经过我们）；
 *   - 索引只记关键帧。一条记录的**结束偏移**由下一条推出，最后一条由
 *     data_write_off 收尾——因此索引项固定 32 字节，不存长度；
 *   - 崩溃恢复靠扫描 Zone C 的 pack 链（00 00 01 BA），丢掉末尾半帧。
 *     写路径全程追加，不需要 fsync 也能保证"已刷盘的完整 pack 一定可读"。
 */
#ifndef ZMS_SRC_STORAGE_ZS_SLOT_H
#define ZMS_SRC_STORAGE_ZS_SLOT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZS_SLOT_HEADER_BYTES 512u
#define ZS_SLOT_ZONE_A_BYTES 4096u
#define ZS_SLOT_MAGIC 0x544C535Au /* "ZSLT" */
#define ZS_SLOT_VERSION 1u
#define ZS_KEYFRAME_ENTRY_BYTES 32u
/** 流标识最大长度（含结尾 0）。 */
#define ZS_SLOT_STREAM_MAX 64u
/** Zone B 默认 1MB -> 32640 条关键帧（1 秒 1 个 I 帧约 9 小时）。 */
#define ZS_SLOT_DEFAULT_INDEX_BYTES (1024u * 1024u)

enum {
    ZS_SLOT_FREE = 0,
    ZS_SLOT_WRITING = 1,
    ZS_SLOT_SEALED = 2
};

/** 槽位头，固定 512 字节，写盘前对其计算 CRC32。 */
typedef struct zs_slot_header {
    uint32_t magic;
    uint16_t version;
    uint16_t state;
    uint32_t channel_id;
    uint32_t slot_seq;
    uint64_t file_bytes;
    uint64_t data_start_off;   /**< Zone C 起点 = ZONE_A + index_bytes */
    uint64_t data_end_off;     /**< Zone C 终点 = file_bytes */
    uint64_t data_write_off;   /**< 下一个 pack 的写入偏移 */
    uint64_t seg_start_utc_ms; /**< 首帧 UTC */
    uint64_t seg_end_utc_ms;   /**< 末帧 UTC */
    uint32_t keyframe_count;
    uint32_t index_capacity;
    uint32_t index_bytes;
    uint32_t index_crc;  /**< 仅对有效条目范围计算 */
    uint32_t header_crc; /**< 对本结构前 4 字节以外的部分计算 */
    /** 流标识（如 <设备内码>_<通道内码>），回放侧按它检索，槽位自描述。 */
    char stream[ZS_SLOT_STREAM_MAX];
    uint8_t reserved[ZS_SLOT_HEADER_BYTES - 148u];
} zs_slot_header;

/** 关键帧索引项，固定 32 字节。 */
typedef struct zs_keyframe_entry {
    uint64_t utc_ms;      /**< 关键帧绝对时间 */
    uint64_t pts_90k;     /**< 归一化 64 位 90kHz 时间戳 */
    uint64_t file_offset; /**< 该关键帧第一个 PS pack 的绝对文件偏移 */
    uint32_t event_mask;  /**< 预留：AI 事件位图；一期恒 0 */
    uint32_t reserved;
} zs_keyframe_entry;

typedef struct zs_slot zs_slot;

typedef struct zs_slot_opts {
    uint32_t channel_id;
    uint32_t slot_seq;
    const char *stream;   /**< 流标识（可 NULL），槽位头里存一份供回放检索 */
    uint64_t slot_bytes;  /**< 文件总大小（需 > ZONE_A + index_bytes + 64KB） */
    uint32_t index_bytes; /**< Zone B 大小，0 取默认 1MB */
} zs_slot_opts;

/**
 * 创建并格式化一个新槽位（覆盖同名文件）。
 * @return 句柄；失败 NULL。
 */
zs_slot *zs_slot_create(const char *path, const zs_slot_opts *opts);

/** 打开已有槽位（只读头，不扫描数据区）。 */
zs_slot *zs_slot_open(const char *path);
void zs_slot_close(zs_slot *s);

/**
 * 崩溃恢复：从 Zone C 起点扫描 PS pack 链，把 data_write_off 截断到最后
 * 一个完整 pack，并按数据区终点裁剪索引。幂等，可对已封口槽位调用。
 * @param seal_after 非 0 时恢复完直接把槽位置为 SEALED
 * @return 0 成功；-1 头损坏/IO 错误
 */
int zs_slot_recover(zs_slot *s, int seal_after);

/** 追加一段 PS 字节（调用方保证是 1..n 个完整 pack）。 */
int zs_slot_append(zs_slot *s, const void *buf, size_t len);

/**
 * 记录一个关键帧。
 * @param file_offset 必须是即将追加的 pack 的绝对偏移（通常等于当前写入偏移）
 */
int zs_slot_add_keyframe(zs_slot *s, uint64_t utc_ms, int64_t pts_90k, uint64_t file_offset,
                         uint32_t event_mask);

/** 封口：写回头（state=SEALED），之后不允许再写。 */
int zs_slot_seal(zs_slot *s);

/**
 * 回收为 FREE：清空索引计数与写指针，state=FREE。
 * 槽位文件保持预分配大小（不删除、不 unlink），由池在下一次申请时复用。
 */
int zs_slot_reset(zs_slot *s);

/** 把 FREE 槽位切回 WRITING 并绑定通道/流（复用时调用）。 */
int zs_slot_begin(zs_slot *s, uint32_t channel_id, const char *stream);

/** 把 stdio 缓冲刷到 OS（不 fsync，恢复不依赖 fsync）。 */
int zs_slot_flush(zs_slot *s);

/**
 * 查找 utc_ms 之前（含）最近的关键帧。
 * @return 0 找到；1 没有（早于首个关键帧）；-1 参数/IO 错误
 */
int zs_slot_find(const zs_slot *s, uint64_t utc_ms, zs_keyframe_entry *out);

/** 按下标取关键帧。@return 0 成功；-1 越界/错误 */
int zs_slot_get_keyframe(const zs_slot *s, uint32_t idx, zs_keyframe_entry *out);

uint64_t zs_slot_write_offset(const zs_slot *s);
uint64_t zs_slot_data_start(const zs_slot *s);
uint64_t zs_slot_data_end(const zs_slot *s);
uint64_t zs_slot_file_bytes(const zs_slot *s);
uint64_t zs_slot_segment_start_ms(const zs_slot *s);
uint64_t zs_slot_segment_end_ms(const zs_slot *s);
uint32_t zs_slot_keyframe_count(const zs_slot *s);
uint32_t zs_slot_index_capacity(const zs_slot *s);
uint32_t zs_slot_state(const zs_slot *s);
uint32_t zs_slot_channel_id(const zs_slot *s);
/** 槽位头里的流标识（可能为空串）。 */
const char *zs_slot_stream(const zs_slot *s);
/** Zone C 剩余可写字节。 */
uint64_t zs_slot_remaining(const zs_slot *s);

/** 从数据区读一段（回放/校验用）；off 是绝对文件偏移。 */
int zs_slot_pread(const zs_slot *s, void *buf, size_t len, uint64_t off);

/**
 * 单个 PS pack 的长度（字节）；返回 -1 表示当前位置不是可判定的合法 pack。
 * 解析规则：pack header(00 00 01 BA, 含 stuffing) 后紧跟一个 PES/系统头，
 * 用 PES_packet_length 定位 pack 末尾。PES 长度为 0（无界）视为不可判定。
 */
long zs_ps_pack_len(const uint8_t *buf, size_t avail);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_STORAGE_ZS_SLOT_H */
