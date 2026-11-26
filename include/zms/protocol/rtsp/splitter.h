/**
 * @file splitter.h
 * @brief RTSP 输入分流。
 */
#ifndef ZMS_PROTOCOL_RTSP_SPLITTER_H
#define ZMS_PROTOCOL_RTSP_SPLITTER_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include "parser.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*zms_rtsp_on_message_cb)(const zms_rtsp_message *msg, void *user);
typedef void (*zms_rtsp_on_rtp_cb)(uint8_t channel, const uint8_t *data, size_t len, void *user);

typedef struct zms_rtsp_splitter zms_rtsp_splitter;

typedef struct zms_rtsp_splitter_opts {
    zms_rtsp_on_message_cb on_message;
    zms_rtsp_on_rtp_cb on_rtp;
    void *user;
} zms_rtsp_splitter_opts;

/** 创建 RTSP 输入分流器；opts 必须提供回调。@return 新实例，内存不足返回 NULL。 */
ZMS_API zms_rtsp_splitter *zms_rtsp_splitter_create(const zms_rtsp_splitter_opts *opts);
/** 销毁分流器。 */
ZMS_API void zms_rtsp_splitter_destroy(zms_rtsp_splitter *s);
/** 开关 RTP 交织（`$` 通道）解析；关闭时仅处理 RTSP 文本消息。 */
ZMS_API void zms_rtsp_splitter_enable_rtp(zms_rtsp_splitter *s, int on);
/**
 * 喂入原始字节流，累积并分发完整消息/RTP 包到回调。
 * @return ZN_OK；ZN_ERR_BUFFER_TOO_SMALL 时（启用 RTP）自动从 `$` 重同步；其余为错误
 */
ZMS_API zn_err_t zms_rtsp_splitter_input(zms_rtsp_splitter *s, const void *data, size_t len);
/** 清空内部组帧状态与暂存消息（重连/切换前调用）。 */
ZMS_API void zms_rtsp_splitter_reset(zms_rtsp_splitter *s);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_RTSP_SPLITTER_H */
