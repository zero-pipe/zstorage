/**
 * @file zcs_export.h
 * @brief 导出宏：ZCS_API（稳定 ABI）与 ZCS_LOCAL（hidden）。
 */
#ifndef ZCS_ZCS_EXPORT_H
#define ZCS_ZCS_EXPORT_H

/** 稳定 ABI 导出宏：共享库构建下导出/导入公共符号，静态构建下展开为空。 */
#if defined(ZCS_BUILD_SHARED)
#if defined(_WIN32) && defined(ZCS_EXPORTS)
#define ZCS_API __declspec(dllexport)
#elif defined(_WIN32)
#define ZCS_API __declspec(dllimport)
#elif defined(__GNUC__)
#define ZCS_API __attribute__((visibility("default")))
#else
#define ZCS_API
#endif
#else
#define ZCS_API
#endif

/* Internal cross-TU symbol that must stay out of the shared-library ABI.
 * Visibility is harmless for static archives; on Windows an un-annotated
 * function is not exported anyway. */
#if defined(__GNUC__)
#define ZCS_LOCAL __attribute__((visibility("hidden")))
#else
#define ZCS_LOCAL
#endif

#endif /* ZCS_ZCS_EXPORT_H */
