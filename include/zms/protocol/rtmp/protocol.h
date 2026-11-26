/**
 * @file protocol.h
 * @brief RTMP 协议接口（握手/chunk/命令/发送）。
 */
#ifndef ZMS_PROTOCOL_RTMP_PROTOCOL_H
#define ZMS_PROTOCOL_RTMP_PROTOCOL_H

#include "zms/media/wire/amf.h"
#include "zms/media/wire/rtmp.h"
#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zn_poller;

typedef struct zms_rtmp_chunk zms_rtmp_chunk;

typedef void (*zms_rtmp_on_chunk_cb)(const zms_rtmp_chunk *chunk, void *user);
typedef void (*zms_rtmp_on_cmd_cb)(const char *cmd, double trans_id, const uint8_t *amf_rest,
                                   size_t amf_rest_len, void *user);

typedef struct zms_rtmp_protocol zms_rtmp_protocol;

typedef struct zms_rtmp_protocol_opts {
    zms_rtmp_on_chunk_cb on_chunk;
    zms_rtmp_on_cmd_cb on_cmd;
    void *user;
} zms_rtmp_protocol_opts;

typedef struct zms_rtmp_chunk {
    uint8_t type_id;
    uint32_t stream_id;
    uint32_t tag_dts_ms;
    const uint8_t *body;
    size_t body_size;
} zms_rtmp_chunk;

/**
 * 创建 RTMP 协议对象（服务端/客户端共用），初始握手状态为等待 C0+C1。
 * @param opts chunk/cmd 回调；可为 NULL（无回调）
 * @return 新对象；内存不足返回 NULL
 */
ZMS_API zms_rtmp_protocol *zms_rtmp_protocol_create(const zms_rtmp_protocol_opts *opts);
/** 握手已完成（客户端侧），直接进入 chunk 解析 */
ZMS_API zms_rtmp_protocol *zms_rtmp_protocol_create_established(const zms_rtmp_protocol_opts *opts);
/** 销毁协议对象：归还各 chunk stream 缓冲与 rx 缓冲、释放握手状态。对 NULL 安全。 */
ZMS_API void zms_rtmp_protocol_destroy(zms_rtmp_protocol *p);
/** 绑定会话 poller：rx_buf / cs.body 走 poller 本地池（须在首次 input 前调用） */
ZMS_API void zms_rtmp_protocol_set_poller(zms_rtmp_protocol *p, struct zn_poller *poller);
/**
 * 投喂接收字节：先推进握手，完成后按 chunk 解析并驱动 on_chunk/on_cmd。
 * 解析错误会重置全部 chunk stream 并丢弃积压（不返回错误），以容忍坏包。
 * @return 仅在参数非法或握手错误时返回错误；正常（含坏包丢弃）返回 ZN_OK
 */
ZMS_API zn_err_t zms_rtmp_protocol_input(zms_rtmp_protocol *p, const void *data, size_t len);
/**
 * 生成握手响应 S0+S1+S2 写入 out。
 * @param cap 至少 1 + 2*ZMS_RTMP_HS_BODY
 * @return ZN_OK；容量/参数不足 ZN_ERR_INVALID；握手状态未就绪 ZN_ERR_STATE
 */
ZMS_API zn_err_t zms_rtmp_protocol_send_handshake_s0s1s2(zms_rtmp_protocol *p, uint8_t *out,
                                                          size_t cap, size_t *out_len);
/**
 * 发送一条 AMF0 命令（cmd + trans_id + null + amf_body），封装成 command chunk。
 * @return ZN_ERR_INVALID 参数缺失；ZN_ERR_NOMEM AMF 编码溢出内部 4KB 缓冲；否则透传 send_chunk
 */
ZMS_API zn_err_t zms_rtmp_protocol_send_invoke(zms_rtmp_protocol *p, const char *cmd,
                                                double trans_id, uint32_t msg_stream_id,
                                                const uint8_t *amf_body, size_t amf_len,
                                                uint8_t *out, size_t cap, size_t *out_len);
/** 同 send_invoke；omit_null=1 时不插入第 3 个 AMF null（connect 的对象必须在该位置） */
ZMS_API zn_err_t zms_rtmp_protocol_send_invoke_ex(zms_rtmp_protocol *p, const char *cmd,
                                                   double trans_id, uint32_t msg_stream_id,
                                                   const uint8_t *amf_body, size_t amf_len,
                                                   uint8_t *out, size_t cap, size_t *out_len,
                                                   int omit_null);
/**
 * 发送一条原始 chunk（含分块与 basic header）。
 * @param type_id    消息类型 id（如 ZMS_RTMP_MSG_CMD / AUDIO / VIDEO）
 * @param tag_dts_ms 时间戳（ms）
 * @param out        输出缓冲；容量不足时通过 out_len 给出所需字节（不写入）
 * @return 透传 chunk 层错误码
 */
ZMS_API zn_err_t zms_rtmp_protocol_send_chunk(zms_rtmp_protocol *p, uint8_t type_id,
                                               uint32_t stream_id, uint32_t tag_dts_ms,
                                               const void *body, size_t len, uint8_t *out,
                                               size_t cap, size_t *out_len);
/**
 * 查询是否需要回握手响应；命中时消费该标志，仅返回一次 1。
 * 事件驱动模型下用它决定是否调用 send_handshake_s0s1s2。
 */
ZMS_API int zms_rtmp_protocol_handshake_pending(const zms_rtmp_protocol *p);
/**
 * 查询握手是否刚完成、需要发送服务端 init 控制消息；命中时消费标志，仅返回一次 1。
 */
ZMS_API int zms_rtmp_protocol_handshake_complete(const zms_rtmp_protocol *p);
/** 生成服务端 init 控制消息（Window Ack / Set Peer BW / Chunk Size 等），写入 out。 */
ZMS_API zn_err_t zms_rtmp_protocol_send_server_init(zms_rtmp_protocol *p, uint8_t *out, size_t cap,
                                                     size_t *out_len);
/** 设置出站 chunk size；0 还原为默认值。应在发 SetChunkSize 后与对端保持一致。 */
ZMS_API void zms_rtmp_protocol_set_out_chunk_size(zms_rtmp_protocol *p, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_PROTOCOL_RTMP_PROTOCOL_H */
