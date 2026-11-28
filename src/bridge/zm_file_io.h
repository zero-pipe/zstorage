/**
 * @file zm_file_io.h
 * @brief 文件型 `zm_io` 适配器（ZM reader 的 pull 模式）。
 */
#ifndef ZCS_SRC_BRIDGE_ZM_FILE_IO_H
#define ZCS_SRC_BRIDGE_ZM_FILE_IO_H

#include "zcs/zcs_export.h"
#include "zm/core/io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 以只读方式打开 path 并填充 io。@return 0 成功，-1 失败。 */
ZCS_LOCAL int zcs_zm_file_io_open(const char *path, zm_io *io);

/** 关闭由 zcs_zm_file_io_open 打开的 io（释放 FILE 与 ctx）。 */
ZCS_LOCAL void zcs_zm_file_io_close(zm_io *io);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_SRC_BRIDGE_ZM_FILE_IO_H */
