/**
 * @file webhook_client.h
 * @brief webhook 事件通知（仅通知，不含鉴权）。
 */
#ifndef ZCS_OPS_API_WEBHOOK_WEBHOOK_CLIENT_H
#define ZCS_OPS_API_WEBHOOK_WEBHOOK_CLIENT_H

/**
 * HTTP WebHook 事件通知（对应 ZLMediaKit on_* 通知子集，供 Java/WVP 对接）。
 *
 * 只做**通知**：on_stream_changed / on_stream_none_reader / on_play_stop /
 * on_server_started / on_record_mp4。全部以非阻塞 HTTP POST 在主 poller 上完成，
 * 不占线程、不做 DNS、不阻塞事件循环。
 *
 * 推流/播放鉴权**不属于本模块**：业务鉴权在上游 web 后台完成并签发 token，
 * 流媒体只做本地验签，见 zcs/core/auth.h。
 */
#include "zcs/ops/config/config.h"
#include "zcs/zcs_export.h"
#include "zcs/core/event.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化（poller 用于承载非阻塞 HTTP）。进程启动调用一次。 */
ZCS_API void zcs_webhook_init(zn_poller *poller, const zcs_config *cfg);
/** 关闭：等待在途通知收尾并释放。 */
ZCS_API void zcs_webhook_fini(void);

/**
 * 启动自注册（on_server_started）：把本节点地址、各协议实际监听端口、
 * 协议能力开关（rtsp/rtmp/hls/srt/webrtc）与 API secret 上报给 web 平台，
 * 平台据此自动登记媒体节点，无需手工添加。在监听服务起来、端口确定后调用。
 */
ZCS_API void zcs_webhook_server_started(const zcs_config *cfg, const zcs_media_server_ports *ports);

/**
 * 启用心跳/续注册：按 [hook] keepalive_interval_sec 周期向 on_server_keepalive
 * 上报同样的注册信息。可在平台尚未就绪时用来自愈式补注册。留空地址则不发。
 */
ZCS_API void zcs_webhook_start_keepalive(const zcs_config *cfg,
                                         const zcs_media_server_ports *ports);
/** 停止心跳（幂等）。 */
ZCS_API void zcs_webhook_stop_keepalive(void);

/** on_stream_changed regist=true */
ZCS_API void zcs_webhook_on_stream_register(zcs_media_source *src, zcs_media_origin origin);
/** on_stream_changed regist=false */
ZCS_API void zcs_webhook_on_stream_unregister(zcs_media_source *src, zcs_media_origin origin);

/**
 * 播放会话断开通知（on_play_stop）。
 * @param duration_ms 会话持续时长（毫秒）；0 表示未知，字段将被省略
 */
ZCS_API void zcs_webhook_on_play_stop(const zcs_media_tuple *tuple, const char *player_schema,
                                      uint64_t duration_ms);
/** 无人观看（响应可含 "close":true 触发关流） */
ZCS_API void zcs_webhook_on_none_reader(const zcs_media_tuple *tuple);

/** 直播 MP4 切片完成通知（on_record_mp4） */
ZCS_API void zcs_webhook_on_record_mp4(const char *app, const char *stream, const char *file_name,
                                       const char *file_path, const char *folder, int64_t file_size,
                                       int64_t start_time, float time_len);

/* ── 云存储节点自注册（与取流 ZCS 节点分离，平台落独立表）────────────── */

/**
 * 存储节点自注册（on_storage_started）：上报节点地址、HTTP/RTSP 端口、secret 与
 * 容量/已用（来自 [storage] 与录制器统计）。留空地址则不发。启动时调用一次。
 */
ZCS_API void zcs_webhook_storage_started(const zcs_config *cfg, const zcs_media_server_ports *ports);
/**
 * 开启存储节点心跳（on_storage_keepalive）：按 [hook] keepalive_interval_sec 周期
 * 重发同样的注册体，顺带刷新容量/已用。可与 ZCS 节点心跳并行。
 */
ZCS_API void zcs_webhook_start_storage_keepalive(const zcs_config *cfg,
                                                 const zcs_media_server_ports *ports);

/** PS 槽位封口完成通知（on_record_ps）：平台据此登记 zws_cloud_record。 */
ZCS_API void zcs_webhook_on_record_ps(const char *channel, uint32_t slot, int64_t start_ms,
                                      int64_t end_ms, int64_t duration_ms, int64_t file_size,
                                      const char *file_path, int status);

/** 是否配置了 on_stream_none_reader（供 media_events 调度） */
ZCS_API int zcs_webhook_none_reader_configured(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_OPS_API_WEBHOOK_WEBHOOK_CLIENT_H */
