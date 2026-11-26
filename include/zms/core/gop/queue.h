#ifndef ZMS_CORE_GOP_QUEUE_H
#define ZMS_CORE_GOP_QUEUE_H

/**
 * @file queue.h
 * @brief 直播多协议播放共用的 GOP 环形队列。
 *
 * 每个 source 一条 ES 车道：Annex-B / raw AAC，供 RTSP、HLS、FLV 重封装。
 * 写入时经 es_codec 刷新 ES keyframe；读者独立附着，GOP 索引支持起播 seek。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/core/frame.h"
#include "zms/core/gop/limits.h"
#include "zms/zms_export.h"
#include "zn/util/buf.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_gop_queue zms_gop_queue;
typedef struct zms_gop_reader zms_gop_reader;

/** 环形槽位的公开视图（仅 ES 载荷）。 */
typedef struct zms_gop_slot {
    zms_codec_id codec;
    zms_track_type track;
    /** 入站归一化后的解码时间戳（ms）；驱动 A/V 交织与 pacing。 */
    uint32_t dts_ms;
    /** 显示时间戳；0 表示与 @a dts_ms 相同。 */
    uint32_t pts_ms;
    int keyframe;
    int config_frame;
    int drop_able;
    uint8_t *data;
    size_t len;
} zms_gop_slot;

/**
 * 新建一个 GOP 环形队列（含互斥量、读者注册表、唤醒队列）。
 * 保留 GOP 数/时间窗/字节上限取自进程级默认值（见 set_default_*）。
 * @return 新队列；内存不足返回 NULL。所有权归调用方，用 destroy 释放。
 */
ZMS_API zms_gop_queue *zms_gop_queue_create(void);
/**
 * 销毁队列并释放全部槽位缓冲、配置、唤醒队列与互斥量。
 * 调用前必须保证所有读者已 detach（否则读者游标悬挂）。
 */
ZMS_API void zms_gop_queue_destroy(zms_gop_queue *r);
/**
 * 清空队列内全部帧数据、GOP 索引与视频/音频配置，并重置读写游标。
 * 保留队列对象本身与读者附着关系（适合推流重连时复用）。
 */
ZMS_API void zms_gop_queue_clear(zms_gop_queue *r);

/**
 * @brief 设置新建队列的默认保留 GOP 数（进程级；create 时拷贝到实例）。
 * @param n 钳制到 [1, ZMS_GOP_QUEUE_MAX_GOP]。
 */
ZMS_API void zms_gop_queue_set_default_target_gops(unsigned n);

/** @brief 覆盖单队列保留 GOP 数（已存在的 source）。 */
ZMS_API void zms_gop_queue_set_target_gops(zms_gop_queue *r, unsigned n);

/**
 * @brief 设置新建队列的默认时间窗缓存（毫秒；0=仅按 GOP 数/容量裁剪）。
 */
ZMS_API void zms_gop_queue_set_default_cache_ms(unsigned ms);

/** @brief 覆盖单队列时间窗缓存（毫秒）。 */
ZMS_API void zms_gop_queue_set_cache_ms(zms_gop_queue *r, unsigned ms);

/** 每路 GOP 缓存的字节上限（0=不限）；超过即丢最旧 GOP。 */
ZMS_API void zms_gop_queue_set_default_byte_budget(size_t bytes);
/** 覆盖单队列字节上限（0=不限）；持锁写入，超过即丢最旧 GOP（至少保留当前 GOP）。 */
ZMS_API void zms_gop_queue_set_byte_budget(zms_gop_queue *r, size_t bytes);
/** 当前环内帧数据字节数（统计/诊断用）。 */
ZMS_API size_t zms_gop_queue_bytes(const zms_gop_queue *r);

/** @return 最慢读者与写端之间未读槽位数。 */
ZMS_API size_t zms_gop_queue_pending_count(const zms_gop_queue *r);

/** @return 当前保留的 GOP 段数（空则为 0）。 */
ZMS_API size_t zms_gop_queue_gop_count(const zms_gop_queue *r);

/** @return 所有附着读者中的最大读滞后（无读者则为 0）。 */
ZMS_API size_t zms_gop_queue_max_reader_lag(const zms_gop_queue *r);

/**
 * @brief 为下一写槽准备可写 buf（优先复用 refcnt==1 的槽位缓冲，否则共享池 alloc）。
 * @note 填好后配合 @ref zms_gop_queue_write_buf；失败返回 NULL。
 */
ZMS_API zn_buf *zms_gop_queue_alloc_write(zms_gop_queue *r, size_t size);

/**
 * @brief 向环形缓冲写入一帧归一化 ES。
 * @param frame H.264/H.265 Annex-B、raw AAC 等。
 * @note 单写者（入站线程）；读者使用独立游标。
 */
ZMS_API zn_err_t zms_gop_queue_write(zms_gop_queue *r, const zms_frame *frame);

/**
 * @brief 接管 buf 写入一帧（所有权转入环形缓冲）。
 * @param buf 载荷缓冲；成功后调用方不得再使用或 unref。
 */
ZMS_API zn_err_t zms_gop_queue_write_buf(zms_gop_queue *r, zn_buf *buf, const zms_frame *frame);

/** 排空环级合并唤醒（遗留 / PLAY 订阅）。 */
ZMS_API int zms_gop_queue_drain_wake(zms_gop_queue *r);

/** 排空该读者按会话的唤醒（MPSC + wake_pending）。 */
ZMS_API int zms_gop_reader_drain_wake(const zms_gop_reader *rd);

/** 拷贝保存视频参数集（SPS/PPS/VPS 等）；成功后置 cache_started 与 has_video。 */
ZMS_API zn_err_t zms_gop_queue_set_video_config(zms_gop_queue *r, const void *data, size_t len);
/** 拷贝保存音频配置（如 AAC AudioSpecificConfig）；data 为 NULL/len=0 时清空。 */
ZMS_API zn_err_t zms_gop_queue_set_audio_config(zms_gop_queue *r, const void *data, size_t len);
/**
 * 取视频配置。
 * @param len 输出长度，可为 NULL
 * @return 指向队列内定长双缓冲的指针（队列存活期间始终有效，不会因清流/重推而释放，
 *         跨线程借用安全）；无配置返回 NULL。并发更新至多让返回值落在上一代缓冲上。
 */
ZMS_API const uint8_t *zms_gop_queue_video_config(const zms_gop_queue *r, size_t *len);
/**
 * 取音频配置。
 * @param len 输出长度，可为 NULL
 * @return 指向队列内定长双缓冲的指针，语义同 video_config。
 */
ZMS_API const uint8_t *zms_gop_queue_audio_config(const zms_gop_queue *r, size_t *len);

/**
 * @brief 加锁复制视频配置（快照语义；需要“长度与内容严格一致”时使用）。
 * @return 已复制字节数；不可用时为 0。
 */
ZMS_API size_t zms_gop_queue_copy_video_config(zms_gop_queue *r, uint8_t *buf, size_t cap);
/** 加锁复制音频配置到 buf；缓冲不足或配置为空返回 0。 */
ZMS_API size_t zms_gop_queue_copy_audio_config(zms_gop_queue *r, uint8_t *buf, size_t cap);
/** 由音频配置推断音频编解码器；无配置返回 ZMS_CODEC_INVALID。 */
ZMS_API zms_codec_id zms_gop_queue_audio_codec(const zms_gop_queue *r);

/** 当前附着 gop_queue 的读者数（PLAY session 上限见 media_limits.h） */
ZMS_API int zms_gop_reader_count(const zms_gop_queue *r);
/**
 * 附着读者，游标置于缓冲最旧位置（不跳到直播 GOP 尾）。
 * @return 新读者；队列为空或内存不足返回 NULL。用 detach 释放。
 */
ZMS_API zms_gop_reader *zms_gop_reader_attach(zms_gop_queue *r);

/** 附着并从缓冲头部顺序读（非直播 GOP 尾）。 */
ZMS_API zms_gop_reader *zms_gop_reader_attach_beginning(zms_gop_queue *r);
/**
 * 解除附着并释放读者及其持有缓冲。
 * 从队列读者表摘除后 free；调用后 rd 不得再用。
 */
ZMS_API void zms_gop_reader_detach(zms_gop_reader *rd);

/** 读游标向直播边缘前移（仅向前裁剪；起播 bootstrap 用 seek_gop_key）。 */
ZMS_API void zms_gop_reader_seek_live(zms_gop_reader *rd);

/** 定位到当前 GOP 内首个同步关键帧（RTMP/HTTP-FLV 起播）。 */
ZMS_API void zms_gop_reader_seek_gop_key(zms_gop_reader *rd);

/** 先到直播边缘，再落到最近 GOP 同步点（RTSP attach use_gop）。 */
ZMS_API void zms_gop_reader_seek_live_key(zms_gop_reader *rd);

/**
 * @brief seek_live_key 后推进到窗口内首个 IDR；若尚无则停在同步点等待。
 */
ZMS_API void zms_gop_reader_seek_live_idr(zms_gop_reader *rd);

/**
 * @brief RTSP bootstrap：仅当前 GOP 最后一个 IDR（无 CRA 回退）。
 */
ZMS_API void zms_gop_reader_seek_rtsp_idr(zms_gop_reader *rd);

/** 从 read_idx 向前找到首个解码起播帧（如 H.265 IDR）。 */
ZMS_API void zms_gop_reader_seek_decode_start(zms_gop_reader *rd);

/** 快照 read_idx 处槽位（诊断 / bootstrap 附着检查）。 */
ZMS_API int zms_gop_reader_slot_at_read(const zms_gop_reader *rd, zms_gop_slot *out);

/** @return 非零表示读游标落在解码起播帧上。 */
ZMS_API int zms_gop_reader_at_decode_start(const zms_gop_reader *rd);

/** @return 读者相对写端的帧滞后（0 表示已追上直播）。 */
ZMS_API size_t zms_gop_reader_lag(const zms_gop_reader *rd);

/**
 * @brief 读取下一槽位并推进游标。
 * @return 成功为 1，无数据为 0，错误为负。
 */
ZMS_API int zms_gop_reader_read(zms_gop_reader *rd, zms_gop_slot *out);

/**
 * @brief 按 dts_ms peek/交织（RTSP PLAY / FLV 的 A/V 复用）。
 * @param max_skew_ms 一路相对另一路允许的最大超前（ms）。
 */
ZMS_API int zms_gop_reader_peek_muxed(zms_gop_reader *rd, zms_gop_slot *out, uint32_t max_skew_ms);
/**
 * 按 dts_ms 交织取出下一个槽位并推进内部 mux 缓冲（RTSP PLAY / FLV 复用）。
 * @param max_skew_ms 一路相对另一路允许的最大超前（ms）；0 用默认值
 * @return 成功为 1，无数据为 0，错误为负
 * @note 与 peek_muxed 的关键差异：会消费该帧（reader 持有其引用，下次调用自动释放）。
 */
ZMS_API int zms_gop_reader_read_muxed(zms_gop_reader *rd, zms_gop_slot *out, uint32_t max_skew_ms);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CORE_GOP_QUEUE_H */
