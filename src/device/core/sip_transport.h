/**
 * @file sip_transport.h
 * @brief SIP 传输接口：UDP / TCP（后续 TLS），统一分帧与收发。
 *
 * 设备端当前使用「UDP 监听」或「TCP 客户端」；分帧规则为 SIP
 * Content-Length。底层复用 ZN（udp_server / tcp_client），协议层不直接碰 socket。
 */
#ifndef ZCS_SRC_DEVICE_CORE_SIP_TRANSPORT_H
#define ZCS_SRC_DEVICE_CORE_SIP_TRANSPORT_H

#include "zn/poller/poller.h"
#include "zn/poller/poller_pool.h"
#include "zn/zn_errno.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_sip_transport_mode {
    ZCS_SIP_TRANSPORT_UDP = 0,
    ZCS_SIP_TRANSPORT_TCP = 1,
    ZCS_SIP_TRANSPORT_TLS = 2,
} zcs_sip_transport_mode;

typedef struct zcs_sip_transport zcs_sip_transport;

typedef void (*zcs_sip_message_fn)(void *user, const char *peer_ip, uint16_t peer_port,
                                   const void *data, size_t len);
typedef void (*zcs_sip_state_fn)(void *user, int connected);

typedef struct zcs_sip_transport_opts {
    zn_poller *poller;
    /** TCP/TLS 服务端需要 */
    zn_poller_pool *poller_pool;
    /** UDP 监听地址 / 端口；TCP 客户端模式忽略端口 */
    const char *bind_host;
    uint16_t local_port;
    int mode; /**< ZCS_SIP_TRANSPORT_* */
    /** 1 = 监听（平台连入）；0 = 客户端（设备连平台） */
    int server_mode;
    /** TCP 客户端：平台 SIP 地址 */
    const char *remote_host;
    uint16_t remote_port;
    /** TLS 客户端：CA / 校验；TLS 服务端：cert/key */
    const char *tls_ca;
    int tls_verify_peer;
    const char *tls_cert;
    const char *tls_key;
    zcs_sip_message_fn on_message;
    zcs_sip_state_fn on_state;
    void *user;
} zcs_sip_transport_opts;

zcs_sip_transport *zcs_sip_transport_create(const zcs_sip_transport_opts *opts);
/** 启动监听 / 发起连接。 */
int zcs_sip_transport_start(zcs_sip_transport *t);
void zcs_sip_transport_stop(zcs_sip_transport *t);
void zcs_sip_transport_destroy(zcs_sip_transport *t);

/** 实际本地端口（UDP 监听端口 / TCP 已连通时的本地端口；未知为 0）。 */
uint16_t zcs_sip_transport_local_port(const zcs_sip_transport *t);
int zcs_sip_transport_is_connected(const zcs_sip_transport *t);

/** UDP 按 peer 发送；TCP 使用当前连接（peer 参数忽略）。 */
int zcs_sip_transport_send(zcs_sip_transport *t, const char *peer_ip, uint16_t peer_port,
                           const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_DEVICE_CORE_SIP_TRANSPORT_H */
