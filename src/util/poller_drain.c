/**
 * @file poller_drain.c
 * @brief poller 异步 drain。
 */
#include "zms/util/poller_drain.h"
#include "zn/poller/poller.h"
#include "zn/platform.h"

void zms_poller_pool_drain_async(zn_poller_pool *pool)
{
    unsigned i;
    unsigned n;

    if (!pool) {
        return;
    }
    n = zn_poller_pool_size(pool);
    for (i = 0; i < n; ++i) {
        zn_poller *p = zn_poller_pool_at(pool, i);
        if (!p) {
            continue;
        }
        (void)zn_poller_wake(p);
        zn_poller_process_pending(p);
    }
}

void zms_poller_pool_drain_async_wait(zn_poller_pool *pool, unsigned wait_ms)
{
    zms_poller_pool_drain_async(pool);
    if (wait_ms > 0) {
        zn_sleep_ms((int)wait_ms);
    }
    zms_poller_pool_drain_async(pool);
}
