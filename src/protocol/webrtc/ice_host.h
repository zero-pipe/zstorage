/**
 * @file ice_host.h
 * @brief WebRTC ICE 运行时状态与会话侧回调：协议 ICE 模块与会话层的唯一边界。
 *
 * 背景：`ice.c` 曾直接包含 `session_internal.h` 并读写 `zcs_webrtc_session` 的
 * 十几个字段，导致协议层反向依赖会话层、目录无法拆分。现在 ICE 模块只依赖本
 * 结构：发送口、对端地址、gather 进度与会话事件回调；会话层把它内嵌在
 * `zcs_webrtc_session` 中并填充。
 *
 * 约定：
 *  - `id`/`owner` 等指针的生命周期由会话保证（内嵌即同生共死）；
 *  - ICE 模块只写 `peer_*`、`gather_*`、`ctx`，不碰其它字段；
 *  - 事件（收到媒体/连通）一律回调会话层，不在协议层分派业务。
 */
#ifndef ZCS_SRC_PROTOCOL_WEBRTC_ICE_HOST_H
#define ZCS_SRC_PROTOCOL_WEBRTC_ICE_HOST_H

#include "zn/net/udp_server.h"
#include "zn/poller/poller.h"
#include <stddef.h>
#include <stdint.h>

struct zcs_webrtc_ice;

#define ZCS_WEBRTC_ICE_UFRAG_LEN 32
#define ZCS_WEBRTC_ICE_PWD_LEN 64
#define ZCS_WEBRTC_PEER_IP_LEN 64

typedef struct zcs_webrtc_ice_host {
    /** 会话指针：回调时原样带回。 */
    void *owner;
    zn_poller *poller;
    zn_udp_server *udp;
    /** 日志标识（指向会话的 id 缓冲，生命周期随会话）。 */
    const char *id;
    /** 主动连接模式（WHEP/WHIP client）：full ICE controlling。 */
    int client_mode;
    /** 1 = 发布会话（WHIP 入站）。 */
    int publish;

    /** 选中的对端（媒体直接 sendto；由 ICE 模块写入）。 */
    char peer_ip[ZCS_WEBRTC_PEER_IP_LEN];
    uint16_t peer_port;
    int peer_known;

    /** gather 进度（信令层据此判断候选是否就绪）。 */
    int gather_expected;
    int gather_done;

    /** ICE 上下文；由 zcs_webrtc_ice_create 填写。 */
    struct zcs_webrtc_ice *ctx;

    /** 收到非 STUN 数据（DTLS/SRTP/RTP）：交给会话层分派。 */
    void (*on_data)(void *owner, const void *data, size_t len);
    /** 连通性检查成功：会话层据此启动 DTLS 等后续动作。 */
    void (*on_connected)(void *owner);
} zcs_webrtc_ice_host;

#endif /* ZCS_SRC_PROTOCOL_WEBRTC_ICE_HOST_H */
