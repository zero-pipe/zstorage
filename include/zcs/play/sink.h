/**
 * @file sink.h
 * @brief 播放输出端统一取数契约：muxer → 会话发送。
 *
 * 所有把媒体序列化成线上格式、再由会话发出去的 muxer（HTTP-FLV live/VOD、
 * HTTP-TS、后续新增协议）都遵循同一套约定，会话侧只实现一份“取数 + 扩容 + 发送”。
 *
 * 约定：
 *   next(muxer, out, cap, &out_len)
 *     > 0            产出一块数据，*out_len 有效
 *     == 0           暂时没有数据
 *     ZCS_SINK_NEED_BIGGER
 *                    cap 不够；*out_len 给出所需字节数。数据已暂存在 muxer 内，
 *                    用不小于该值的缓冲重试即可拿到同一块数据（不会丢帧）。
 *     其它 < 0       错误，会话应断开
 *
 * 为什么不让 muxer 自己 malloc：分配策略（栈、poller 本地缓冲池、上限）属于
 * 会话侧，muxer 只该回答“我要多少字节”。这样新增协议时不需要再复制一份扩容逻辑。
 *
 * HTTP-TS 是例外：MPEG-TS 固定 188 字节包，单包永远放得下，因此不需要扩容，
 * 但它的 next() 仍遵守 >0 / ==0 / <0 的返回值语义。
 */
#ifndef ZCS_PLAY_SINK_H
#define ZCS_PLAY_SINK_H

#include "zcs/zcs_export.h"
#include "zn/util/buf.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 输出缓冲不足：*out_len 返回所需字节数，数据留在 muxer 内待重试。 */
#define ZCS_SINK_NEED_BIGGER (-2)

/** muxer 取数回调。 */
typedef int (*zcs_sink_next_fn)(void *muxer, uint8_t *out, size_t cap, size_t *out_len);

/**
 * 按契约取一块数据，缓冲不够时扩容并重试一次。
 *
 * @param next       muxer 取数回调
 * @param muxer      回调上下文
 * @param pol        该会话所属 poller；用于从本地缓冲池分配（NULL 用全局池）
 * @param init_cap   首次尝试的缓冲大小
 * @param max_cap    允许扩容到的上限；muxer 要求更大时按错误处理
 * @param out        成功时返回持有数据的 zn_buf（调用方负责 zn_buf_unref）
 * @param out_len    数据长度
 * @return >0 有数据；0 暂无数据；<0 错误（缓冲超上限时为 ZN_ERR_BUFFER_TOO_SMALL）
 */
ZCS_API int zcs_sink_pull(zcs_sink_next_fn next, void *muxer, struct zn_poller *pol,
                          size_t init_cap, size_t max_cap, zn_buf **out, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PLAY_SINK_H */
