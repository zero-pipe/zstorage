#ifndef ZM_ZM_EXPORT_H
#define ZM_ZM_EXPORT_H

/**
 * @file zm_export.h
 * @brief ZM 导出宏。静态库构建时为空。
 */
#if defined(_WIN32) && defined(ZM_BUILD_SHARED)
#if defined(ZM_EXPORTS)
#define ZM_API __declspec(dllexport)
#else
#define ZM_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) && defined(ZM_BUILD_SHARED)
#define ZM_API __attribute__((visibility("default")))
#else
#define ZM_API
#endif

#endif /* ZM_ZM_EXPORT_H */
