/**
 * @file test_poller_timer.c
 * @brief zn_poller timer 生命周期：跨线程取消（引用计数）与回调内自取消。
 *
 * 覆盖修复点：跨线程 cancel 任务与定时器到期并发时的 UAF / 定时器泄漏，
 * 以及回调持业务锁时跨线程 cancel 的锁序（回调在 timer_mtx 外执行）。
 */
#include "zn/poller/poller.h"
#include "zn/util/timer.h"
#include "zb/os/thread.h"
#include "zb/os/platform.h"
#include "zb/os/sync.h"

#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static volatile int g_stop;
static volatile int g_fired;
static volatile int g_long_fired;
static volatile int g_lock_fired;
static zn_poller_timer *g_self;
static zb_mutex *g_user_mu;

/* 看门狗：锁序回归用例若发生死锁，在这里兜底退出，避免 ctest 永久挂起。
 * 测试正常结束时置 g_done，看门狗自行退出并被 join
 * （进程带活线程退出在部分平台会崩）。 */
#define LOCK_ORDER_DEADLINE_MS 5000

static volatile int g_done;

static void watchdog_main(void *user)
{
    int waited = 0;

    (void)user;
    while (!g_done && waited < LOCK_ORDER_DEADLINE_MS) {
        zb_sleep_ms(20);
        waited += 20;
    }
    if (!g_done) {
        fprintf(stderr, "check failed: poller timer lock-order deadlock (watchdog)\n");
        exit(3);
    }
}

static uint64_t on_fire(void *user)
{
    (void)user;
    ++g_fired;
    return 0;
}

static uint64_t on_long_fire(void *user)
{
    (void)user;
    ++g_long_fired;
    return 0;
}

/* 回调内取消自身：回调在锁外执行，cancel 走同线程路径（堆外不摘链）。 */
static uint64_t on_self_cancel(void *user)
{
    (void)user;
    if (g_self) {
        zn_poller_timer_cancel(g_self);
        g_self = NULL;
    }
    ++g_fired;
    return 0;
}

/* 回调取业务锁；修复前回调在 timer_mtx 内执行，跨线程 cancel 会构成 ABBA 死锁。 */
static uint64_t on_lock_fire(void *user)
{
    (void)user;
    if (g_user_mu) {
        zb_mutex_lock(g_user_mu);
        ++g_lock_fired;
        zb_mutex_unlock(g_user_mu);
    }
    return 10;
}

static void poller_main(void *user)
{
    zn_poller_run((zn_poller *)user, &g_stop);
}

int main(void)
{
    zn_poller *p;
    zb_thread *th;
    int i;

    p = zn_poller_create();
    CHECK(p != NULL);
    th = zb_thread_create(poller_main, p);
    CHECK(th != NULL);

    /* 等待 poller 线程进入运行状态。 */
    for (i = 0; i < 200 && !zn_poller_is_running(p); ++i) {
        zb_sleep_ms(1);
    }
    CHECK(zn_poller_is_running(p));

    /* 1) 长定时器跨线程取消后不得触发。 */
    for (i = 0; i < 64; ++i) {
        zn_poller_timer *t = zn_poller_do_delay(p, 5000, on_long_fire, NULL);
        CHECK(t != NULL);
        zn_poller_timer_cancel(t);
    }

    /* 2) 短定时器取消风暴：与到期并发，主要验证无 UAF/泄漏。 */
    for (i = 0; i < 300; ++i) {
        zn_poller_timer *t = zn_poller_do_delay(p, 5, on_fire, NULL);
        CHECK(t != NULL);
        zn_poller_timer_cancel(t);
    }

    /* 3) 回调内自取消。 */
    g_self = zn_poller_do_delay(p, 10, on_self_cancel, NULL);
    CHECK(g_self != NULL);

    /* 4) 锁序回归：业务线程持锁时跨线程取消「回调内会取同一把锁」的定时器。
     *    修复前回调在 timer_mtx 内执行 → cancel 等 timer_mtx、回调等业务锁，ABBA 死锁。 */
    zb_thread *wd = NULL;
    {
        zn_poller_timer *t;

        g_user_mu = zb_mutex_create(ZB_MUTEX_NORMAL);
        CHECK(g_user_mu != NULL);
        wd = zb_thread_create(watchdog_main, NULL);
        CHECK(wd != NULL);
        t = zn_poller_do_delay(p, 5, on_lock_fire, NULL);
        CHECK(t != NULL);
        zb_sleep_ms(50);
        CHECK(g_lock_fired > 0);
        zb_mutex_lock(g_user_mu);
        zn_poller_timer_cancel(t);
        zb_mutex_unlock(g_user_mu);
        /* 定时器 cancel 是异步的：回调可能仍在 poller 线程上取这把锁，
         * 因此不能在 poller 线程结束前销毁（否则锁到已释放的互斥量）。
         * 留到 poller join 之后再销毁。 */
    }

    /* 让已调度的短定时器有机会到期。 */
    zb_sleep_ms(80);

    g_stop = 1;
    zn_poller_wake(p);
    CHECK(zb_thread_join(th) == ZB_OK);
    zb_thread_destroy(th);
    g_done = 1;
    if (wd) {
        CHECK(zb_thread_join(wd) == ZB_OK);
        zb_thread_destroy(wd);
        wd = NULL;
    }
    /* poller 线程已退出，回调不会再执行，此时销毁业务锁才安全。 */
    if (g_user_mu) {
        zb_mutex_destroy(g_user_mu);
        g_user_mu = NULL;
    }
    zn_poller_destroy(p);

    CHECK(g_long_fired == 0);
    CHECK(g_fired >= 1); /* 自取消回调至少执行一次 */
    printf("test_poller_timer: OK (fired=%d)\n", g_fired);
    return 0;
}
