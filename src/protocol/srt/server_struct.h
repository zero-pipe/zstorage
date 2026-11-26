/**
 * @file server_struct.h
 * @brief SRT 服务私有结构。
 */
#ifndef ZMS_SRC_PROTOCOL_SRT_SERVER_STRUCT_H
#define ZMS_SRC_PROTOCOL_SRT_SERVER_STRUCT_H

#include "zn/poller/poller.h"
#include <stdint.h>

#ifdef ZMS_HAVE_SRT
#include <srt/srt.h>
#else
typedef int SRTSOCKET;
#endif

typedef struct zn_timer zn_timer;

typedef struct zms_srt_session zms_srt_session;

typedef struct zms_srt_server {
    zn_poller *poller;
    zn_timer *io_timer;
    int epoll_id;
    SRTSOCKET listen_sock;
    volatile int running;
    uint16_t port;
    int session_serial;
    zms_srt_session *sessions;
} zms_srt_server;

#endif /* ZMS_SRC_PROTOCOL_SRT_SERVER_STRUCT_H */
