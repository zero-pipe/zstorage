/**
 * @file test_znet_tcp_shutdown.c
 * @brief znet tcp_server 在 poller 仍运行时销毁的回归（原 ASan UAF:
 *        session_detach 读到已被 server destroy 释放的 session）。
 */
#include "zn/net/socket.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include "zb/os/platform.h"
#include "zb/os/thread.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static volatile int g_echoed;
static zn_tcp_session *g_session;

static void on_recv(zn_tcp_session *session, const void *data, size_t len, void *user)
{
    (void)user;
    g_session = session;
    if (zn_tcp_session_send(session, data, len) == ZN_OK)
        g_echoed = 1;
}

static zn_err_t wait_connect(zn_socket *sock, int timeout_ms)
{
    int waited = 0;
    while (waited < timeout_ms) {
        zn_err_t err = zn_socket_check_connect(sock);
        if (err == ZN_OK)
            return ZN_OK;
        if (err != ZN_ERR_AGAIN)
            return err;
        zb_sleep_ms(5);
        waited += 5;
    }
    return zn_socket_check_connect(sock);
}

static void async_sender(void *user)
{
    (void)user;
    for (int i = 0; i < 50; ++i)
        (void)zn_tcp_session_send_async(g_session, "x", 1);
}

static int one_round(void)
{
    zn_poller_pool_opts_t popts = { .size = 2, .prefer_current_thread = 0,
        .thread_priority = ZB_THREAD_PRIO_NORMAL };
    zn_tcp_session_ops_t ops = { on_recv, NULL, NULL };
    zn_tcp_server_opts_t sopts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 16,
        .session_ops = &ops,
        .manager_interval_sec = -1.0f
    };
    zn_poller_pool *pool;
    zn_tcp_server *srv;
    zn_socket *cli;
    char out[16];
    int pending = 0;

    pool = zn_poller_pool_create(&popts);
    CHECK(pool != NULL);
    CHECK(zn_poller_pool_start(pool) == ZN_OK);
    sopts.poller_pool = pool;

    srv = zn_tcp_server_create(&sopts);
    CHECK(srv != NULL);
    CHECK(zn_tcp_server_start(srv) == ZN_OK);

    cli = zn_socket_create();
    CHECK(cli != NULL);
    CHECK(zn_socket_connect(cli, "127.0.0.1", zn_tcp_server_port(srv), &pending) == ZN_OK);
    if (pending)
        CHECK(wait_connect(cli, 3000) == ZN_OK);

    g_echoed = 0;
    CHECK(zn_socket_send(cli, "ping", 4) == 4);
    for (int i = 0; i < 200 && !g_echoed; ++i)
        zb_sleep_ms(5);
    CHECK(g_echoed);
    memset(out, 0, sizeof(out));
    /* connect 走的是非阻塞路径，回环上 echo 到达时间不定：
     * 这里按“可能 AGAIN”轮询，而不是假设 recv 会阻塞等待。 */
    {
        int got = 0;
        int waited = 0;

        while (waited < 3000 && got < 4) {
            zn_ssize_t n = zn_socket_recv(cli, out + got, sizeof(out) - (size_t)got);

            if (n > 0) {
                got += (int)n;
                continue;
            }
            if (n != (zn_ssize_t)ZN_ERR_AGAIN) {
                break;
            }
            zb_sleep_ms(5);
            waited += 5;
        }
        CHECK(got == 4);
    }
    CHECK(memcmp(out, "ping", 4) == 0);

    /* 跨线程发送：任务持有 session 引用；随后立即销毁 server，验证引用计数不 UAF。 */
    {
        zb_thread *sender = zb_thread_create(async_sender, NULL);
        CHECK(sender != NULL);
        zb_thread_join(sender);
        zb_thread_destroy(sender);
    }

    /* 关键：poller 仍在运行时销毁 server（旧实现会与回调并发释放 session）。 */
    zn_tcp_server_destroy(srv);
    zn_socket_destroy(cli);
    zn_poller_pool_stop(pool);
    zn_poller_pool_destroy(pool);
    return 0;
}

int main(void)
{
    for (int i = 0; i < 30; ++i) {
        if (one_round() != 0) {
            fprintf(stderr, "round %d failed\n", i);
            return 1;
        }
    }
    printf("test_znet_tcp_shutdown: ok\n");
    return 0;
}
