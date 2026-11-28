#ifndef ZCS_CORE_STREAM_BUFFER_H
#define ZCS_CORE_STREAM_BUFFER_H

/**
 * @file buffer.h
 * @brief 点播播放缓冲（顺序 ES 缓冲，非 GOP 缓存）。
 *
 * 每次播放会话使用独立 reader；vod_reader 在背压下将 sample 泵入缓冲。
 * 不与直播 gop_queue 路径共享。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/gop/queue.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCS_VOD_BUFFER_DEFAULT_CAP 512u

/* 点播 config 定长双缓冲容量（与直播 ZCS_GOP_QUEUE_CONFIG_MAX 同量级）。 */
#define ZCS_VOD_BUFFER_CONFIG_MAX 2048u

typedef struct zcs_vod_buffer zcs_vod_buffer;
typedef struct zcs_vod_buffer_reader zcs_vod_buffer_reader;

/**
 * 创建点播缓冲。
 * @param cap 环形槽位数；传 0 时使用 ZCS_VOD_BUFFER_DEFAULT_CAP。
 * @return 新缓冲；内存不足返回 NULL。
 */
ZCS_API zcs_vod_buffer *zcs_vod_buffer_create(size_t cap);
/** 销毁缓冲并释放其槽位与配置；须确保没有存活的 reader。 */
ZCS_API void zcs_vod_buffer_destroy(zcs_vod_buffer *buf);

/**
 * 写入一帧到环形缓冲（内部加锁）。
 *
 * frame->owned 且容量足够时直接接管其 data，否则复制一份；因此调用方在
 * owned 情况下不再拥有该内存。缓冲满（未消费槽位达 cap）时返回
 * ZN_ERR_BUFFER_TOO_SMALL，调用方应等待 reader 消费后再写。
 * @return ZN_OK 成功；参数非法 ZN_ERR_INVALID；内存不足 ZN_ERR_NOMEM
 */
ZCS_API zn_err_t zcs_vod_buffer_write(zcs_vod_buffer *buf, const zcs_frame *frame);
/** 复制并保存视频参数集（SPS/PPS/VPS 等），覆盖旧值。 */
ZCS_API zn_err_t zcs_vod_buffer_set_video_config(zcs_vod_buffer *buf, const void *data,
                                                  size_t len);
/** 复制并保存音频参数集（AudioSpecificConfig 等），覆盖旧值。 */
ZCS_API zn_err_t zcs_vod_buffer_set_audio_config(zcs_vod_buffer *buf, const void *data,
                                                  size_t len);
/** 清空所有槽位与配置（复用缓冲前调用），不释放缓冲本体。 */
ZCS_API void zcs_vod_buffer_reset(zcs_vod_buffer *buf);

/** 将 reader 移到最早未消费帧（prepare_play / seek 之后）。 */
ZCS_API void zcs_vod_buffer_reader_seek_beginning(zcs_vod_buffer_reader *rd);
/**
 * 取视频参数集。
 * @param len 输出长度
 * @return 借用的指针（归属缓冲，随缓冲重置/销毁失效）；无配置返回 NULL
 */
ZCS_API const uint8_t *zcs_vod_buffer_video_config(const zcs_vod_buffer *buf, size_t *len);
/** 取音频参数集；返回借用的指针（归属缓冲），无配置返回 NULL。 */
ZCS_API const uint8_t *zcs_vod_buffer_audio_config(const zcs_vod_buffer *buf, size_t *len);
/** 当前可读、尚未被最慢 reader 消费的帧数。 */
ZCS_API size_t zcs_vod_buffer_pending(const zcs_vod_buffer *buf);

/** @return 非零表示缓冲含 H.264 IDR（PLAY 前扫描）。 */
ZCS_API int zcs_vod_buffer_has_h264_idr(const zcs_vod_buffer *buf);

/** @return 非零表示缓冲含 H.264/H.265 视频 sync 关键帧。 */
ZCS_API int zcs_vod_buffer_has_video_sync_key(const zcs_vod_buffer *buf);

/**
 * @param from_beginning 1 = 从最旧帧读；0 = 从附着游标读。
 */
ZCS_API zcs_vod_buffer_reader *zcs_vod_buffer_reader_attach(zcs_vod_buffer *buf,
                                                            int from_beginning);
/** 释放 reader；对 NULL 调用安全。不会销毁底层的缓冲。 */
ZCS_API void zcs_vod_buffer_reader_detach(zcs_vod_buffer_reader *rd);
/**
 * 读取下一个槽位并前移游标（muxed：音视频交错）。
 * @return 1 取到帧（slot->data 借用自缓冲，下次 advance 前有效）；0 已无新帧。
 */
ZCS_API int zcs_vod_buffer_reader_read_muxed(zcs_vod_buffer_reader *rd, zcs_gop_slot *slot);
/** 同 read_muxed，但不前移游标（用于预看）。@return 1 有帧，0 无。 */
ZCS_API int zcs_vod_buffer_reader_peek_muxed(zcs_vod_buffer_reader *rd, zcs_gop_slot *slot);

/**
 * @brief 加锁 peek/复制 ES（可抵御并发 mp4 泵覆盖）。
 */
ZCS_API int zcs_vod_buffer_reader_peek_muxed_es(zcs_vod_buffer_reader *rd, zcs_gop_slot *slot,
                                                uint8_t **es_buf, size_t *es_cap,
                                                struct zn_poller *pol);
/** 前移 reader 游标（消费当前 peek 的槽位），并推进缓冲 consumed_seq。 */
ZCS_API void zcs_vod_buffer_reader_advance(zcs_vod_buffer_reader *rd);
/** 将游标前移到下一个视频关键帧（seek 后对齐 IDR）。 */
ZCS_API void zcs_vod_buffer_reader_seek_video_key(zcs_vod_buffer_reader *rd);

/**
 * @brief 在缓冲窗口内按媒体时间 seek。
 * @return 1 成功；实际位置写入 @a out_ms。
 */
ZCS_API int zcs_vod_buffer_reader_seek_ms(zcs_vod_buffer_reader *rd, uint64_t ms, uint64_t *out_ms);
/** 该 reader 尚未消费的帧数（缓冲积压量，用于背压判断）。 */
ZCS_API size_t zcs_vod_buffer_reader_lag(zcs_vod_buffer_reader *rd);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_STREAM_BUFFER_H */
