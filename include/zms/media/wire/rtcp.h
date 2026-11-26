/**
 * @file rtcp.h
 * @brief RTCP SR/RR/复合包编解码。
 */
#ifndef ZMS_MEDIA_WIRE_RTCP_H
#define ZMS_MEDIA_WIRE_RTCP_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zms_rtcp_sr {
    uint32_t ssrc;
    uint32_t ntp_sec;
    uint32_t ntp_frac;
    uint32_t rtp_ts;
    uint32_t packet_count;
    uint32_t octet_count;
} zms_rtcp_sr;

typedef struct zms_rtcp_rr {
    uint32_t ssrc;
    uint32_t fraction_lost;
    uint32_t cumulative_lost;
    uint32_t highest_seq;
    uint32_t jitter;
    uint32_t lsr;
    uint32_t dlsr;
} zms_rtcp_rr;

/**
 * 解析一个 RTCP SR 包（需 >=28 字节，版本 2、PT=200）。
 * @return ZN_OK 成功；长度不足或类型不符返回 INVALID。
 */
ZMS_API zn_err_t zms_rtcp_parse_sr(const uint8_t *data, size_t len, zms_rtcp_sr *out);
/**
 * 解析一个 RTCP RR 包（需 >=32 字节，PT=201）。
 * @param out fraction_lost/highest_seq/jitter 字段在长度足够时填充
 * @return ZN_OK 成功；长度不足或类型不符返回 INVALID。
 */
ZMS_API zn_err_t zms_rtcp_parse_rr(const uint8_t *data, size_t len, zms_rtcp_rr *out);
/**
 * 构造 28 字节 RTCP SR 包（单发送者报告）。
 * @return 写入字节数（28）；out 为空或 cap<28 返回 0。
 */
ZMS_API size_t zms_rtcp_build_sr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t ntp_sec,
                                 uint32_t ntp_frac, uint32_t rtp_ts, uint32_t packet_count,
                                 uint32_t octet_count);
/**
 * 构造 32 字节 RTCP RR 包（单接收报告，lsr/dlsr 置零）。
 * @return 写入字节数（32）；out 为空或 cap<32 返回 0。
 */
ZMS_API size_t zms_rtcp_build_rr(uint8_t *out, size_t cap, uint32_t ssrc, uint32_t sender_ssrc,
                                 uint32_t fraction_lost, uint32_t highest_seq, uint32_t jitter);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_WIRE_RTCP_H */
