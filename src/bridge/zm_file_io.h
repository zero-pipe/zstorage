/**
 * @file zm_file_io.h
 * @brief 文件型 `zm_io` 适配器（ZM reader 的 pull 模式）。
 */
#ifndef ZMS_SRC_BRIDGE_ZM_FILE_IO_H
#define ZMS_SRC_BRIDGE_ZM_FILE_IO_H

#include "zms/zms_export.h"
#include "zm/core/io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 以只读方式打开 path 并填充 io。@return 0 成功，-1 失败。 */
ZMS_LOCAL int zms_zm_file_io_open(const char *path, zm_io *io);

/** 关闭由 zms_zm_file_io_open 打开的 io（释放 FILE 与 ctx）。 */
ZMS_LOCAL void zms_zm_file_io_close(zm_io *io);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_SRC_BRIDGE_ZM_FILE_IO_H */
