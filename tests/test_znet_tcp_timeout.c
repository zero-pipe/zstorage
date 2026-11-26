/**
 * @file test_znet_tcp_timeout.c
 * @brief znet tcp_server 会话超时与连接上限：握手超时、空闲超时、单 IP 上限。
 */
#include "zn/net/socket.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller_pool.h"
#include "zb/os/platform.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static volatile int g_errors;

static void on_error(zn_tcp_session *session, void *user)
{
    (void)session;
    (void)user;
    ++g_errors;
}

static zn_err_t wait_connect(zn_socket *sock, int timeout_ms)
{
    int waited = 0;

    while (waited < timeout_ms) {
        zn_err_t err = zn_socket_check_connect(sock);

        if (err == ZN_OK) {
            return ZN_OK;
        }
        if (err != ZN_ERR_AGAIN) {
            return err;
        }
        zb_sleep_ms(5);
        waited += 5;
    }
    return zn_socket_check_connect(sock);
}

static zn_socket *connect_cli(uint16_t port)
{
    zn_socket *cli = zn_socket_create();
    int pending = 0;

    if (!cli) {
        return NULL;
    }
    if (zn_socket_connect(cli, "127.0.0.1", port, &pending) != ZN_OK) {
        zn_socket_destroy(cli);
        return NULL;
    }
    if (pending && wait_connect(cli, 3000) != ZN_OK) {
        zn_socket_destroy(cli);
        return NULL;
    }
    return cli;
}

/* 不发送任何字节，等待握手超时把会话关掉（on_error 计数）。 */
static int test_handshake_timeout(void)
{
    zn_poller_pool_opts_t popts = {.size = 1,
                                   .prefer_current_thread = 0,
                                   .thread_priority = ZB_THREAD_PRIO_NORMAL};
    zn_tcp_session_ops_t ops = {NULL, on_error, NULL};
    zn_tcp_server_opts_t sopts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 8,
        .session_ops = &ops,
        .manager_interval_sec = 0.02f,
        .handshake_timeout_ms = 300,
    };
    zn_poller_pool *pool;
    zn_tcp_server *srv;
    zn_socket *cli;
    int i;

    g_errors = 0;
    pool = zn_poller_pool_create(&popts);
    CHECK(pool != NULL);
    CHECK(zn_poller_pool_start(pool) == ZN_OK);
    sopts.poller_pool = pool;
    srv = zn_tcp_server_create(&sopts);
    CHECK(srv != NULL);
    CHECK(zn_tcp_server_start(srv) == ZN_OK);

    cli = connect_cli(zn_tcp_server_port(srv));
    CHECK(cli != NULL);

    for (i = 0; i < 400 && g_errors == 0; ++i) {
        zb_sleep_ms(5);
    }
    CHECK(g_errors >= 1);

    zn_socket_destroy(cli);
    zn_tcp_server_destroy(srv);
    zn_poller_pool_stop(pool);
    zn_poller_pool_destroy(pool);
    return 0;
}

/* 先发一次再停手：空闲超时应关闭会话。 */
static int test_idle_timeout(void)
{
    zn_poller_pool_opts_t popts = {.size = 1,
                                   .prefer_current_thread = 0,
                                   .thread_priority = ZB_THREAD_PRIO_NORMAL};
    zn_tcp_session_ops_t ops = {NULL, on_error, NULL};
    zn_tcp_server_opts_t sopts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 8,
        .session_ops = &ops,
        .manager_interval_sec = 0.02f,
        .idle_timeout_ms = 300,
    };
    zn_poller_pool *pool;
    zn_tcp_server *srv;
    zn_socket *cli;
    int i;

    g_errors = 0;
    pool = zn_poller_pool_create(&popts);
    CHECK(pool != NULL);
    CHECK(zn_poller_pool_start(pool) == ZN_OK);
    sopts.poller_pool = pool;
    srv = zn_tcp_server_create(&sopts);
    CHECK(srv != NULL);
    CHECK(zn_tcp_server_start(srv) == ZN_OK);

    cli = connect_cli(zn_tcp_server_port(srv));
    CHECK(cli != NULL);
    CHECK(zn_socket_send(cli, "x", 1) == 1);
    zb_sleep_ms(100);
    CHECK(g_errors == 0); /* 刚发过数据，不该超时 */

    for (i = 0; i < 400 && g_errors == 0; ++i) {
        zb_sleep_ms(5);
    }
    CHECK(g_errors >= 1);

    zn_socket_destroy(cli);
    zn_tcp_server_destroy(srv);
    zn_poller_pool_stop(pool);
    zn_poller_pool_destroy(pool);
    return 0;
}

/* 单 IP 上限：同一来源的第 N+1 个连接被拒绝。 */
static int test_per_ip_limit(void)
{
    zn_poller_pool_opts_t popts = {.size = 1,
                                   .prefer_current_thread = 0,
                                   .thread_priority = ZB_THREAD_PRIO_NORMAL};
    zn_tcp_session_ops_t ops = {NULL, NULL, NULL};
    zn_tcp_server_opts_t sopts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 8,
        .session_ops = &ops,
        .manager_interval_sec = -1.0f,
        .max_sessions_per_ip = 2,
    };
    zn_poller_pool *pool;
    zn_tcp_server *srv;
    zn_socket *c1;
    zn_socket *c2;
    zn_socket *c3;
    char buf[16];
    int closed = 0;
    int i;

    pool = zn_poller_pool_create(&popts);
    CHECK(pool != NULL);
    CHECK(zn_poller_pool_start(pool) == ZN_OK);
    sopts.poller_pool = pool;
    srv = zn_tcp_server_create(&sopts);
    CHECK(srv != NULL);
    CHECK(zn_tcp_server_start(srv) == ZN_OK);

    c1 = connect_cli(zn_tcp_server_port(srv));
    c2 = connect_cli(zn_tcp_server_port(srv));
    CHECK(c1 != NULL && c2 != NULL);
    zb_sleep_ms(100);

    c3 = connect_cli(zn_tcp_server_port(srv));
    /* 超出上限：服务端 accept 后立即关闭；客户端探测应看到 EOF/错误。 */
    if (c3) {
        for (i = 0; i < 200 && !closed; ++i) {
            zn_ssize_t n = zn_socket_recv(c3, buf, sizeof(buf));

            if (n == 0 || (n < 0 && n != (zn_ssize_t)ZN_ERR_AGAIN)) {
                closed = 1;
            }
            zb_sleep_ms(5);
        }
    }
    CHECK(closed);

    if (c3) {
        zn_socket_destroy(c3);
    }
    zn_socket_destroy(c1);
    zn_socket_destroy(c2);
    zn_tcp_server_destroy(srv);
    zn_poller_pool_stop(pool);
    zn_poller_pool_destroy(pool);
    return 0;
}


/* 服务端只发不收：客户端从不发送字节，但服务端持续下发数据——
 * 这是 HTTP-FLV/VOD 的典型形态，空闲超时按双向活动判定，不得误杀。 */
static volatile int g_server_errors2;
static void on_error2(zn_tcp_session *session, void *user)
{
    (void)session;
    (void)user;
    ++g_server_errors2;
}
static void on_manager2(zn_tcp_session *session, void *user)
{
    static const char payload[] = "frame";
    (void)user;
    (void)zn_tcp_session_send(session, payload, sizeof(payload) - 1);
}

static int test_tx_only_not_idle(void)
{
    zn_poller_pool_opts_t popts = {.size = 1,
                                   .prefer_current_thread = 0,
                                   .thread_priority = ZB_THREAD_PRIO_NORMAL};
    zn_tcp_session_ops_t ops = {NULL, on_error2, on_manager2};
    zn_tcp_server_opts_t sopts = {
        .host = "127.0.0.1",
        .port = 0,
        .backlog = 8,
        .session_ops = &ops,
        .manager_interval_sec = 0.02f,
        .idle_timeout_ms = 800,
    };
    zn_poller_pool *pool;
    zn_tcp_server *srv;
    zn_socket *cli;
    char buf[64];
    int i;

    g_server_errors2 = 0;
    pool = zn_poller_pool_create(&popts);
    CHECK(pool != NULL);
    CHECK(zn_poller_pool_start(pool) == ZN_OK);
    sopts.poller_pool = pool;
    srv = zn_tcp_server_create(&sopts);
    CHECK(srv != NULL);
    CHECK(zn_tcp_server_start(srv) == ZN_OK);

    cli = connect_cli(zn_tcp_server_port(srv));
    CHECK(cli != NULL);
    /* 客户端不发任何字节，只读；持续 2s（远超 800ms idle）。
     * ASan/负载下调度抖动可能延迟首个 on_manager 发送，余量要够。 */
    for (i = 0; i < 200; ++i) {
        (void)zn_socket_recv(cli, buf, sizeof(buf));
        zb_sleep_ms(10);
    }
    CHECK(g_server_errors2 == 0); /* 未被误杀 */

    zn_socket_destroy(cli);
    zn_tcp_server_destroy(srv);
    zn_poller_pool_stop(pool);
    zn_poller_pool_destroy(pool);
    return 0;
}

int main(void)
{
    CHECK(test_handshake_timeout() == 0);
    CHECK(test_idle_timeout() == 0);
    CHECK(test_per_ip_limit() == 0);
    CHECK(test_tx_only_not_idle() == 0);
    printf("test_znet_tcp_timeout: ok\n");
    return 0;
}
