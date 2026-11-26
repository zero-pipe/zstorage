#ifndef ZM_CORE_MEM_H
#define ZM_CORE_MEM_H

/**
 * @file mem.h
 * @brief 与分配器解耦的引用计数载荷句柄。
 *
 * `owner == NULL` 表示借用：`unref` 不释放，生产者保证同步回调期间有效。
 * `retain` 可为 NULL（单所有者，不可跨线程共享）。
 * ZM 不依赖 zn_buf；ZMS 用 `zm_mem_wrap(zn_buf, zn_buf_ref, zn_buf_unref, ...)`
 * 做零拷贝包装。
 */
#include "zm/core/types.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zm_mem {
    void *owner;
    void (*retain)(void *owner);
    void (*release)(void *owner);
    uint8_t *data;
    size_t len;
    size_t cap;
} zm_mem;

/** 借用一段外部内存；不持有、不释放。 */
ZM_API zm_mem zm_mem_borrow(const void *data, size_t len);

/**
 * 包装一个外部所有者。**不额外 retain**：调用方把已有的一份所有权交给句柄。
 * @param retain 可 NULL（单所有者）
 * @param release 释放/归还所有者；可为 NULL（借用）
 */
ZM_API zm_mem zm_mem_wrap(void *owner, void (*retain)(void *owner),
                             void (*release)(void *owner), void *data, size_t len, size_t cap);

/** 增加引用（借用句柄为 no-op）。 */
ZM_API void zm_mem_ref(zm_mem *m);

/** 释放引用并把句柄清零（借用句柄只清零）。 */
ZM_API void zm_mem_unref(zm_mem *m);

/** 用 ZM 默认分配器分配 cap 字节，len=0。 */
ZM_API int zm_mem_alloc(size_t cap, zm_mem *out);

/** 用 ZM 默认分配器分配并复制 len 字节，len=len。 */
ZM_API int zm_mem_dup(const void *data, size_t len, zm_mem *out);

/**
 * 保证 cap 足够。仅对 ZM 默认分配器或可复制的载荷安全：
 * 不足时分配新块、复制 len 字节、释放旧引用。
 */
ZM_API int zm_mem_reserve(zm_mem *m, size_t cap);

/** 设置 len（不超过 cap）。 */
ZM_API void zm_mem_set_len(zm_mem *m, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_MEM_H */
