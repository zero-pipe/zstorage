#ifndef ZCS_PLAY_HLS_IO_H
#define ZCS_PLAY_HLS_IO_H

#include <stddef.h>

/** 初始化 HLS 异步文件写线程（单 worker，保证全局写入顺序）。 */
void zcs_hls_io_init(void);
/** 停止并等待队列排空。 */
void zcs_hls_io_fini(void);
/** 排队写文件；save_dir/name 内部拷贝，data 会被复制。 */
void zcs_hls_io_enqueue(const char *save_dir, const char *name, const void *data, size_t len);

#endif /* ZCS_PLAY_HLS_IO_H */
