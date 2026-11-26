#ifndef ZM_CORE_IO_H
#define ZM_CORE_IO_H

/**
 * @file io.h
 * @brief 字节流输入 / 输出抽象。
 *
 * reader 用 zm_io（文件、内存、socket 由宿主提供）；writer 用 zm_sink。
 * 回调都在调用方线程同步执行；sink->write 必须在下一次调用前消费完 data。
 */
#include "zm/core/types.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zm_io {
    void *ctx;
    int (*read)(void *ctx, void *buf, size_t len); /**< >0 字节, 0 EOF, <0 error */
    int (*seek)(void *ctx, int64_t off, int whence);
    int64_t (*tell)(void *ctx);
    void (*close)(void *ctx);
} zm_io;

typedef struct zm_sink {
    void *ctx;
    int (*write)(void *ctx, const void *data, size_t len);
    int (*seek)(void *ctx, int64_t off);
    int (*tell)(void *ctx, int64_t *off);
    void (*close)(void *ctx);
} zm_sink;

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_IO_H */
