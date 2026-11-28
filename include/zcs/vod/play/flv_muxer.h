/**
 * @file flv_muxer.h
 * @brief VOD HTTP-FLV 复用器。
 */
#ifndef ZCS_VOD_PLAY_FLV_MUXER_H
#define ZCS_VOD_PLAY_FLV_MUXER_H

struct zcs_media_source;
struct zcs_vod_buffer_reader;
struct zcs_vod_flv_index;
struct zcs_egress_clock;
#include "zcs/media/container/flv/flv_index.h"
#include "zcs/zcs_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_vod_flv_muxer zcs_vod_flv_muxer;

/** 点播（共享 vod_buffer reader） */
ZCS_API zcs_vod_flv_muxer *zcs_vod_flv_muxer_create(struct zcs_media_source *src);
/** 外部传入 vod_buffer_reader（如 vod_play_lane） */
ZCS_API zcs_vod_flv_muxer *zcs_vod_flv_muxer_create_reader(struct zcs_media_source *src,
                                                           struct zcs_vod_buffer_reader *rd);
/** 重新绑定媒体源（借用，不转移所有权）；用于源信息晚到的场景。 */
ZCS_API void zcs_vod_flv_muxer_bind_source(zcs_vod_flv_muxer *m, struct zcs_media_source *src);
struct zn_poller;
/** 绑定 I/O poller：使 tag/es 工作区走 poller 本地池，并传给内部 egress pipeline。 */
ZCS_API void zcs_vod_flv_muxer_bind_poller(zcs_vod_flv_muxer *m, struct zn_poller *pol);
/** 销毁复用器：释放 index view、按需 detach reader、归还缓冲、销毁 egress pipeline。对 NULL 安全。 */
ZCS_API void zcs_vod_flv_muxer_destroy(zcs_vod_flv_muxer *m);
/**
 * 启动输出 FLV header + onMetaData 前的准备工作：确定音视频轨道、arm 播放时钟。
 * @param out      输出缓冲
 * @param has_audio/has_video 声明的轨道（与源实际轨道取并）
 * @param out_len  实际写入长度
 * @return ZN_OK；参数非法 ZN_ERR_INVALID
 */
ZCS_API zn_err_t zcs_vod_flv_muxer_start(zcs_vod_flv_muxer *m, int has_audio, int has_video,
                                          uint8_t *out, size_t cap, size_t *out_len);

/**
 * 配置视频/音频参数集与总时长（构建 onMetaData 与首个 sequence 头用）。
 * @param video_cfg/audio_cfg 借用指针，调用方保证生命周期覆盖复用器
 * @param duration_ms 覆盖 onMetaData 的时长；0 表示由源推导
 */
ZCS_API void zcs_vod_flv_muxer_configure(zcs_vod_flv_muxer *m, const uint8_t *video_cfg,
                                         size_t video_len, const uint8_t *audio_cfg,
                                         size_t audio_len, uint64_t duration_ms);
/** 设置关键帧索引视图（接管所有权，旧 view 会被释放）；用于 onMetaData 的关键帧表。 */
ZCS_API void zcs_vod_flv_muxer_set_index_view(zcs_vod_flv_muxer *m, zcs_vod_flv_index_view *view);
/** 设置完整关键帧索引（借用，不接管所有权）；与 set_index_view 二选一。 */
ZCS_API void zcs_vod_flv_muxer_set_index_full(zcs_vod_flv_muxer *m,
                                              const struct zcs_vod_flv_index *idx);
/** 开启/关闭按真实时间节奏输出（HTTP-FLV 点播实时播放）；关闭时尽快洪泛。 */
ZCS_API void zcs_vod_flv_muxer_set_http_realtime_pace(zcs_vod_flv_muxer *m, int on);
/**
 * 跳转到指定位置：重锁播放时钟 epoch、清待发与时间戳、设 catchup 突发。
 * 无 reader 时为空操作；之后应等待关键帧再恢复常规节奏。
 */
ZCS_API void zcs_vod_flv_muxer_seek(zcs_vod_flv_muxer *m, uint32_t ms);
/** 标记 metadata 与音视频配置为首个 sequence 头均已发送，跳过 bootstrap 阶段。 */
ZCS_API void zcs_vod_flv_muxer_skip_bootstrap(zcs_vod_flv_muxer *m);
/** 使用外部提供的播放时钟（借用）；为空时内部自建。 */
ZCS_API void zcs_vod_flv_muxer_bind_play_clock(zcs_vod_flv_muxer *m, struct zcs_egress_clock *clk);
/** 设置追赶突发帧数（<=0 视为 0，即不追赶）。 */
ZCS_API void zcs_vod_flv_muxer_set_catchup(zcs_vod_flv_muxer *m, int catchup);
/** 查询视频是否已 armed（已发出可解码的视频起始点）。 */
ZCS_API int zcs_vod_flv_muxer_video_armed(const zcs_vod_flv_muxer *m);
/** 查询当前 catchup 突发剩余帧数（0 表示已处于常规节奏）。 */
ZCS_API int zcs_vod_flv_muxer_catchup(const zcs_vod_flv_muxer *m);

/**
 * 产出一个 FLV tag（onMetaData / config / 媒体帧按顺序）。
 * @param out     输出缓冲；容量不足返回 ZCS_SINK_NEED_BIGGER 并通过 out_len 给出所需字节，
 *                调用方换更大缓冲重试即可（暂存帧不会丢）
 * @return 1 产出成功；0 暂无数据；-1 错误；ZCS_SINK_NEED_BIGGER 缓冲不足
 */
ZCS_API int zcs_vod_flv_muxer_next(zcs_vod_flv_muxer *m, uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_VOD_PLAY_FLV_MUXER_H */
