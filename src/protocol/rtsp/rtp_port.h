/**
 * @file rtp_port.h
 * @brief RTP over UDP 端口池。
 */
#ifndef ZMS_SRC_PROTOCOL_RTSP_RTP_PORT_H
#define ZMS_SRC_PROTOCOL_RTSP_RTP_PORT_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_rtp_port_pool zms_rtp_port_pool;

typedef struct zms_rtp_port_pool_opts {
    /** 偶数 RTP 端口下界（含） */
    uint16_t range_start;
    /** 偶数 RTP 端口上界（含），须 >= range_start 且为偶数 */
    uint16_t range_end;
} zms_rtp_port_pool_opts;

/** 默认范围 30000–39998（步长 2，约 5000 对） */
ZMS_API zms_rtp_port_pool *zms_rtp_port_pool_create(const zms_rtp_port_pool_opts *opts);
ZMS_API void zms_rtp_port_pool_destroy(zms_rtp_port_pool *pool);

/** 分配一对端口：rtp 为偶数，*rtcp_port = *rtp_port + 1 */
ZMS_API zn_err_t zms_rtp_port_pool_acquire(zms_rtp_port_pool *pool, uint16_t *rtp_port, uint16_t *rtcp_port);

/** 按 RTP 偶数端口释放（须为本池 acquire 所得） */
ZMS_API void zms_rtp_port_pool_release(zms_rtp_port_pool *pool, uint16_t rtp_port);

ZMS_API unsigned zms_rtp_port_pool_free_count(const zms_rtp_port_pool *pool);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_PROTOCOL_RTSP_RTP_PORT_H */
