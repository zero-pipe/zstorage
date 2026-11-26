/**
 * @file onvif_internal.h
 * @brief ONVIF 客户端内部接口（SOAP/XML/鉴权）。
 */
#ifndef ZMS_SRC_DEVICE_ONVIF_INTERNAL_H
#define ZMS_SRC_DEVICE_ONVIF_INTERNAL_H

#include "zms/device/core/device_xml.h"
#include "zms/device/onvif/onvif.h"
#include "zms/util/http_client.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 鉴权相关缓冲上限（字节）。
 * - 挑战头 WWW-Authenticate: Digest realm/nonce/qop/opaque：实测 < 300B；
 * - 请求头 Authorization: Digest（user+realm+nonce+uri+cnonce+response+opaque）：
 *   实测 < 500B（user/pass 上限 64/128 时最坏约 600B）。
 * 均取 2 倍左右余量；构造失败按错误处理，不发送残缺头。
 */
#define ZMS_ONVIF_USER_MAX 64
#define ZMS_ONVIF_PASS_MAX 128
#define ZMS_ONVIF_CHALLENGE_MAX 512
#define ZMS_ONVIF_AUTH_HDR_MAX 1024
#define ZMS_ONVIF_EXTRA_HDR_MAX 1024

struct zms_onvif_client {
    zms_onvif_device dev;
    char user[ZMS_ONVIF_USER_MAX];
    char pass[ZMS_ONVIF_PASS_MAX];
    int timeout_ms;
    /** 0=未知；1=已确认设备使用 HTTP Digest（后续先用最小请求换挑战） */
    int auth_mode;
};

/* ── SOAP ───────────────────────────────────────────────────────────────── */

#define ZMS_ONVIF_BODY_MAX (64 * 1024)


/** 生成 SOAP 1.2 信封；user/pass 为空时不加 Security 头。返回写入长度（0=失败）。 */
size_t zms_onvif_soap_build(char *out, size_t cap, const char *body_inner, const char *user,
                            const char *pass);

/**
 * 调用设备 SOAP 接口。
 * 自动处理 401：WS-UsernameToken → Digest → Basic。
 * 成功（HTTP 200）返回 0，*out_body 为 malloc 的响应 XML，调用方 free。
 */
int zms_onvif_call(zms_onvif_client *c, const char *xaddr, const char *body_inner,
                   char **out_body);

/** base64 编码（含 NUL）；返回写入字符数，0=失败。 */
size_t zms_onvif_base64(const uint8_t *data, size_t len, char *out, size_t cap);

/** 生成 uuid:xxxxxxxx-xxxx-... 形式的 MessageID */
void zms_onvif_make_msgid(char *out, size_t cap);

/** 当前 UTC 时间（YYYY-MM-DDTHH:MM:SSZ） */
void zms_onvif_utc_now(char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_DEVICE_ONVIF_INTERNAL_H */
