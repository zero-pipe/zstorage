/**
 * @file test_zbase_os.c
 * @brief zbase OSAL：mutex/sem/thread/time 回归。
 */
#include "zb/os/platform.h"
#include "zb/os/sem.h"
#include "zb/os/sync.h"
#include "zb/os/thread.h"

#include <stdio.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct thread_ctx {
    zb_mutex *mtx;
    int counter;
} thread_ctx;

static void thread_worker(void *user)
{
    thread_ctx *ctx = (thread_ctx *)user;
    int i;
    for (i = 0; i < 1000; ++i) {
        zb_mutex_lock(ctx->mtx);
        ++ctx->counter;
        zb_mutex_unlock(ctx->mtx);
    }
}

static int test_mutex(void)
{
    zb_mutex *m = zb_mutex_create(ZB_MUTEX_RECURSIVE);
    zb_mutex *n;

    CHECK(m != NULL);
    CHECK(zb_mutex_lock(m) == ZB_OK);
    CHECK(zb_mutex_lock(m) == ZB_OK);
    CHECK(zb_mutex_unlock(m) == ZB_OK);
    CHECK(zb_mutex_unlock(m) == ZB_OK);
    zb_mutex_destroy(m);

    n = zb_mutex_create(ZB_MUTEX_NORMAL);
    CHECK(n != NULL);
    CHECK(zb_mutex_lock(n) == ZB_OK);
    CHECK(zb_mutex_trylock(n) == ZB_ERR_AGAIN);
    CHECK(zb_mutex_unlock(n) == ZB_OK);
    CHECK(zb_mutex_trylock(n) == ZB_OK);
    CHECK(zb_mutex_unlock(n) == ZB_OK);
    zb_mutex_destroy(n);
    return 0;
}

static int test_sem(void)
{
    zb_sem *s = zb_sem_create(0);

    CHECK(s != NULL);
    CHECK(zb_sem_timedwait(s, 20) == ZB_ERR_TIMEOUT);
    CHECK(zb_sem_post(s, 1) == ZB_OK);
    CHECK(zb_sem_wait(s) == ZB_OK);
    zb_sem_destroy(s);
    return 0;
}

static int show_priority(void);

static int test_thread(void)
{
    thread_ctx ctx;
    zb_thread *t;

    ctx.counter = 0;
    ctx.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    CHECK(ctx.mtx != NULL);
    t = zb_thread_create(thread_worker, &ctx);
    CHECK(t != NULL);
    CHECK(zb_thread_join(t) == ZB_OK);
    zb_thread_destroy(t);
    CHECK(ctx.counter == 1000);
    zb_mutex_destroy(ctx.mtx);
    CHECK(zb_thread_hardware_concurrency() >= 1);
    show_priority();
    return 0;
}

static int show_priority(void)
{
    zb_thread_set_current_priority(ZB_THREAD_PRIO_NORMAL);
    return 0;
}

static int test_time(void)
{
    uint64_t a = zb_monotonic_ms();
    uint64_t b;

    zb_sleep_ms(20);
    b = zb_monotonic_ms();
    CHECK(b >= a);
    CHECK(b - a >= 10);
    CHECK(zb_wall_ms() > 0);
    CHECK(zb_thread_self_id() != 0);
    return 0;
}

int main(void)
{
    CHECK(test_mutex() == 0);
    CHECK(test_sem() == 0);
    CHECK(test_thread() == 0);
    CHECK(test_time() == 0);
    printf("test_zbase_os: ok\n");
    return 0;
}
