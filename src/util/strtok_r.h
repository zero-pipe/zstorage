/**
 * @file strtok_r.h
 * @brief 可重入 strtok_r 兼容实现。
 */
#ifndef ZCS_SRC_UTIL_STRTOK_R_H
#define ZCS_SRC_UTIL_STRTOK_R_H

#include <string.h>

#ifdef _WIN32
#define zcs_strtok_r(str, delim, save) strtok_s((str), (delim), (save))
#else
#define zcs_strtok_r(str, delim, save) strtok_r((str), (delim), (save))
#endif

#endif /* ZCS_SRC_UTIL_STRTOK_R_H */
