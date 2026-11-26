#ifndef ZM_SRC_ATOMIC_H
#define ZM_SRC_ATOMIC_H

/* 仅供 libcore 内部使用；不进入公共头。 */
#if defined(_MSC_VER)
#include <intrin.h>
static __inline int zm_atomic_inc(volatile int *p)
{
    return (int)_InterlockedIncrement((volatile long *)p);
}
static __inline int zm_atomic_dec(volatile int *p)
{
    return (int)_InterlockedDecrement((volatile long *)p);
}
#else
static inline int zm_atomic_inc(volatile int *p)
{
    return __sync_add_and_fetch(p, 1);
}
static inline int zm_atomic_dec(volatile int *p)
{
    return __sync_sub_and_fetch(p, 1);
}
#endif

#endif /* ZM_SRC_ATOMIC_H */
