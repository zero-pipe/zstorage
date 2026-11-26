/**
 * @file zms_have_tls.h
 * @brief 编译期 TLS 能力探测。
 */
#ifndef ZMS_OPS_SERVICE_ZMS_HAVE_TLS_H
#define ZMS_OPS_SERVICE_ZMS_HAVE_TLS_H

/** 与 znet 一致：仅当 CMake 找到 OpenSSL 且 ZN_ENABLE_OPENSSL=ON 时为 1 */
#if defined(ZN_HAVE_OPENSSL) && ZN_HAVE_OPENSSL
#define ZMS_HAVE_PULL_TLS 1
#else
#define ZMS_HAVE_PULL_TLS 0
#endif

#endif /* ZMS_OPS_SERVICE_ZMS_HAVE_TLS_H */
