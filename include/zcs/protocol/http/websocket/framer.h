/**
 * @file framer.h
 * @brief WebSocket 帧编解码。
 */
#ifndef ZCS_PROTOCOL_HTTP_WEBSOCKET_FRAMER_H
#define ZCS_PROTOCOL_HTTP_WEBSOCKET_FRAMER_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 服务端发往客户端的二进制帧头长度（载荷未掩码）。 */
ZCS_API size_t zcs_ws_framer_server_binary_hdr(uint8_t *hdr, size_t cap, size_t payload_len);

/** 客户端帧头最大长度（含 4 字节掩码键）。 */
#define ZCS_WS_FRAMER_CLIENT_HDR_MAX 14u

/**
 * 客户端发往服务端的帧头（FIN + opcode，载荷被掩码）。
 * @param mask  4 字节掩码键；载荷须按 mask[i&3] 异或后才能发送
 * @return 帧头字节数；0 表示 cap 不足或参数非法
 */
ZCS_API size_t zcs_ws_framer_client_hdr(uint8_t *hdr, size_t cap, uint8_t opcode,
                                        const uint8_t mask[4], size_t payload_len);

/** 用掩码键就地异或载荷（发送前调用；同一函数也用于还原，XOR 自逆）。 */
ZCS_API void zcs_ws_framer_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4]);

/** 解析客户端帧；close 时调 on_close，ping 时回 pong。返回已消费字节数。 */
typedef void (*zcs_ws_on_close_fn)(void *user);
typedef int (*zcs_ws_send_fn)(void *user, const void *data, size_t len);
/**
 * 解析并消费缓冲区中的客户端帧（就地跳过载荷，close 帧调 on_close，ping 帧回 pong）。
 * @param send 发送回调（用于回 pong）；可为 NULL 则不回
 * @return 已消费的字节数（可能小于 len，表示末尾有半包需等待更多数据）
 */
ZCS_API size_t zcs_ws_framer_consume_client(const uint8_t *data, size_t len, zcs_ws_send_fn send,
                                            zcs_ws_on_close_fn on_close, void *user);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_HTTP_WEBSOCKET_FRAMER_H */
