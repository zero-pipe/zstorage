/**
 * @file strtok_r.h
 * @brief 可重入 strtok_r 兼容实现。
 */
#ifndef ZMS_SRC_UTIL_STRTOK_R_H
#define ZMS_SRC_UTIL_STRTOK_R_H

#include <string.h>

#ifdef _WIN32
#define zms_strtok_r(str, delim, save) strtok_s((str), (delim), (save))
#else
#define zms_strtok_r(str, delim, save) strtok_r((str), (delim), (save))
#endif

#endif /* ZMS_SRC_UTIL_STRTOK_R_H */
