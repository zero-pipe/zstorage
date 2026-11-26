/**
 * @file zms_export.h
 * @brief 导出宏：ZMS_API（稳定 ABI）与 ZMS_LOCAL（hidden）。
 */
#ifndef ZMS_ZMS_EXPORT_H
#define ZMS_ZMS_EXPORT_H

/** 稳定 ABI 导出宏：共享库构建下导出/导入公共符号，静态构建下展开为空。 */
#if defined(ZMS_BUILD_SHARED)
#if defined(_WIN32) && defined(ZMS_EXPORTS)
#define ZMS_API __declspec(dllexport)
#elif defined(_WIN32)
#define ZMS_API __declspec(dllimport)
#elif defined(__GNUC__)
#define ZMS_API __attribute__((visibility("default")))
#else
#define ZMS_API
#endif
#else
#define ZMS_API
#endif

/* Internal cross-TU symbol that must stay out of the shared-library ABI.
 * Visibility is harmless for static archives; on Windows an un-annotated
 * function is not exported anyway. */
#if defined(__GNUC__)
#define ZMS_LOCAL __attribute__((visibility("hidden")))
#else
#define ZMS_LOCAL
#endif

#endif /* ZMS_ZMS_EXPORT_H */
