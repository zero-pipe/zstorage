#ifndef ZCS_SRC_BRIDGE_ZM_RTP_PS_H
#define ZCS_SRC_BRIDGE_ZM_RTP_PS_H

/**
 * @file zm_rtp_ps.h
 * @brief GB28181 RTP/PS 打包桥（RFC 2250，PS 当作字节流分片）。
 *
 * 输入一个完整 PS pack，按 MTU 分片；每个 RTP 包同步回调输出，
 * 最后一个分片置 marker 位（部分平台依赖 M 判断帧边界）。
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_zm_rtp_ps zcs_zm_rtp_ps;

typedef int (*zcs_zm_rtp_ps_write_fn)(void *user, const uint8_t *rtp, size_t len);

typedef struct zcs_zm_rtp_ps_opts {
    uint8_t payload_type;
    uint32_t ssrc;
    uint16_t seq;
    /** 单个 RTP 包总长上限（含 12 字节头）；0 = 1400。 */
    size_t mtu;
    zcs_zm_rtp_ps_write_fn write;
    void *user;
} zcs_zm_rtp_ps_opts;

zcs_zm_rtp_ps *zcs_zm_rtp_ps_create(const zcs_zm_rtp_ps_opts *opts);
void zcs_zm_rtp_ps_destroy(zcs_zm_rtp_ps *s);

/** 打包一个 PS pack；timestamp 为 90kHz RTP 时间戳。 */
int zcs_zm_rtp_ps_input(zcs_zm_rtp_ps *s, const uint8_t *ps, size_t len, uint32_t timestamp);

/** 最近一次输出的序号（诊断用）。 */
uint16_t zcs_zm_rtp_ps_last_seq(const zcs_zm_rtp_ps *s);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_BRIDGE_ZM_RTP_PS_H */
