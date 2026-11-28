/**
 * @file digest.h
 * @brief RTSP Digest 鉴权。
 */
#ifndef ZCS_PROTOCOL_RTSP_DIGEST_H
#define ZCS_PROTOCOL_RTSP_DIGEST_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zcs_rtsp_auth_scheme {
    ZCS_RTSP_AUTH_NONE = 0,
    ZCS_RTSP_AUTH_DIGEST,
    ZCS_RTSP_AUTH_BASIC,
} zcs_rtsp_auth_scheme;

/** 解析 WWW-Authenticate 头（可含 "Digest ..." 前缀） */
ZCS_API zcs_rtsp_auth_scheme zcs_rtsp_digest_parse_www_auth(const char *hdr, char *realm,
                                                            size_t realm_cap, char *nonce,
                                                            size_t nonce_cap);

/**
 * 生成 Authorization 行（不含 "Authorization: " 前缀与 CRLF）。
 * ANSI 密码：response = md5(md5(user:realm:pass):nonce:md5(method:url))
 */
ZCS_API int zcs_rtsp_digest_build_authorization(const char *method, const char *url,
                                                const char *user, const char *pass,
                                                const char *realm, const char *nonce, char *out,
                                                size_t out_cap);

/** 校验客户端 Authorization（Digest / Basic） */
ZCS_API int zcs_rtsp_digest_verify(const char *method, const char *url, const char *user,
                                   const char *pass, const char *realm, const char *nonce,
                                   const char *authorization);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_PROTOCOL_RTSP_DIGEST_H */
