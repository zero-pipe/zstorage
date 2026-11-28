/**
 * @file zcs_have_tls.h
 * @brief 编译期 TLS 能力探测。
 */
#ifndef ZCS_OPS_SERVICE_ZCS_HAVE_TLS_H
#define ZCS_OPS_SERVICE_ZCS_HAVE_TLS_H

/** 与 znet 一致：仅当 CMake 找到 OpenSSL 且 ZN_ENABLE_OPENSSL=ON 时为 1 */
#if defined(ZN_HAVE_OPENSSL) && ZN_HAVE_OPENSSL
#define ZCS_HAVE_PULL_TLS 1
#else
#define ZCS_HAVE_PULL_TLS 0
#endif

#endif /* ZCS_OPS_SERVICE_ZCS_HAVE_TLS_H */
