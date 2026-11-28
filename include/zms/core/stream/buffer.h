#ifndef ZMS_CORE_STREAM_BUFFER_H
#define ZMS_CORE_STREAM_BUFFER_H

/**
 * @file buffer.h
 * @brief 点播播放缓冲（顺序 ES 缓冲，非 GOP 缓存）。
 *
 * 每次播放会话使用独立 reader；vod_reader 在背压下将 sample 泵入缓冲。
 * 不与直播 gop_queue 路径共享。
 *
 * Copyright (c) zero-media-server
 */
#include "zms/core/gop/queue.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZMS_VOD_BUFFER_DEFAULT_CAP 512u

/* 点播 config 定长双缓冲容量（与直播 ZMS_GOP_QUEUE_CONFIG_MAX 同量级）。 */
#define ZMS_VOD_BUFFER_CONFIG_MAX 2048u

typedef struct zms_vod_buffer zms_vod_buffer;
typedef struct zms_vod_buffer_reader zms_vod_buffer_reader;

/**
 * 创建点播缓冲。
 * @param cap 环形槽位数；传 0 时使用 ZMS_VOD_BUFFER_DEFAULT_CAP。
 * @return 新缓冲；内存不足返回 NULL。
 */
ZMS_API zms_vod_buffer *zms_vod_buffer_create(size_t cap);
/** 销毁缓冲并释放其槽位与配置；须确保没有存活的 reader。 */
ZMS_API void zms_vod_buffer_destroy(zms_vod_buffer *buf);

/**
 * 写入一帧到环形缓冲（内部加锁）。
 *
 * frame->owned 且容量足够时直接接管其 data，否则复制一份；因此调用方在
 * owned 情况下不再拥有该内存。缓冲满（未消费槽位达 cap）时返回
 * ZN_ERR_BUFFER_TOO_SMALL，调用方应等待 reader 消费后再写。
 * @return ZN_OK 成功；参数非法 ZN_ERR_INVALID；内存不足 ZN_ERR_NOMEM
 */
ZMS_API zn_err_t zms_vod_buffer_write(zms_vod_buffer *buf, const zms_frame *frame);
/** 复制并保存视频参数集（SPS/PPS/VPS 等），覆盖旧值。 */
ZMS_API zn_err_t zms_vod_buffer_set_video_config(zms_vod_buffer *buf, const void *data,
                                                  size_t len);
/** 复制并保存音频参数集（AudioSpecificConfig 等），覆盖旧值。 */
ZMS_API zn_err_t zms_vod_buffer_set_audio_config(zms_vod_buffer *buf, const void *data,
                                                  size_t len);
/** 清空所有槽位与配置（复用缓冲前调用），不释放缓冲本体。 */
ZMS_API void zms_vod_buffer_reset(zms_vod_buffer *buf);

/** 将 reader 移到最早未消费帧（prepare_play / seek 之后）。 */
ZMS_API void zms_vod_buffer_reader_seek_beginning(zms_vod_buffer_reader *rd);
/**
 * 取视频参数集。
 * @param len 输出长度
 * @return 借用的指针（归属缓冲，随缓冲重置/销毁失效）；无配置返回 NULL
 */
ZMS_API const uint8_t *zms_vod_buffer_video_config(const zms_vod_buffer *buf, size_t *len);
/** 取音频参数集；返回借用的指针（归属缓冲），无配置返回 NULL。 */
ZMS_API const uint8_t *zms_vod_buffer_audio_config(const zms_vod_buffer *buf, size_t *len);
/** 当前可读、尚未被最慢 reader 消费的帧数。 */
ZMS_API size_t zms_vod_buffer_pending(const zms_vod_buffer *buf);

/** @return 非零表示缓冲含 H.264 IDR（PLAY 前扫描）。 */
ZMS_API int zms_vod_buffer_has_h264_idr(const zms_vod_buffer *buf);

/** @return 非零表示缓冲含 H.264/H.265 视频 sync 关键帧。 */
ZMS_API int zms_vod_buffer_has_video_sync_key(const zms_vod_buffer *buf);

/**
 * @param from_beginning 1 = 从最旧帧读；0 = 从附着游标读。
 */
ZMS_API zms_vod_buffer_reader *zms_vod_buffer_reader_attach(zms_vod_buffer *buf,
                                                            int from_beginning);
/** 释放 reader；对 NULL 调用安全。不会销毁底层的缓冲。 */
ZMS_API void zms_vod_buffer_reader_detach(zms_vod_buffer_reader *rd);
/**
 * 读取下一个槽位并前移游标（muxed：音视频交错）。
 * @return 1 取到帧（slot->data 借用自缓冲，下次 advance 前有效）；0 已无新帧。
 */
ZMS_API int zms_vod_buffer_reader_read_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot);
/** 同 read_muxed，但不前移游标（用于预看）。@return 1 有帧，0 无。 */
ZMS_API int zms_vod_buffer_reader_peek_muxed(zms_vod_buffer_reader *rd, zms_gop_slot *slot);

/**
 * @brief 加锁 peek/复制 ES（可抵御并发 mp4 泵覆盖）。
 */
ZMS_API int zms_vod_buffer_reader_peek_muxed_es(zms_vod_buffer_reader *rd, zms_gop_slot *slot,
                                                uint8_t **es_buf, size_t *es_cap,
                                                struct zn_poller *pol);
/** 前移 reader 游标（消费当前 peek 的槽位），并推进缓冲 consumed_seq。 */
ZMS_API void zms_vod_buffer_reader_advance(zms_vod_buffer_reader *rd);
/** 将游标前移到下一个视频关键帧（seek 后对齐 IDR）。 */
ZMS_API void zms_vod_buffer_reader_seek_video_key(zms_vod_buffer_reader *rd);

/**
 * @brief 在缓冲窗口内按媒体时间 seek。
 * @return 1 成功；实际位置写入 @a out_ms。
 */
ZMS_API int zms_vod_buffer_reader_seek_ms(zms_vod_buffer_reader *rd, uint64_t ms, uint64_t *out_ms);
/** 该 reader 尚未消费的帧数（缓冲积压量，用于背压判断）。 */
ZMS_API size_t zms_vod_buffer_reader_lag(zms_vod_buffer_reader *rd);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_CORE_STREAM_BUFFER_H */
