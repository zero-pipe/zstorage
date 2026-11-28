/**
 * @file amf3_decode.h
 * @brief AMF3 解码：解析 connect 命令对象。
 */
#ifndef ZCS_PROTOCOL_RTMP_AMF3_DECODE_H
#define ZCS_PROTOCOL_RTMP_AMF3_DECODE_H

#include "zcs/zcs_export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 从 RTMP connect 命令 AMF 参数中提取 app / tcUrl（支持 AMF0 / AMF3）。
 * @return 1 成功解析到至少一个 app 或 tcUrl；0 失败
 */
ZCS_API int zcs_amf_extract_connect(const uint8_t *data, size_t len, char *app, size_t app_cap,
                                    char *tc_url, size_t tc_cap);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTMP_AMF3_DECODE_H */
