/**
 * @file test_http_request_reader.c
 * @brief HTTP 请求读取器：分片输入、流水线、Content-Length、超限拒绝、ws 判定。
 *
 * 为什么测它：这是 HTTP 服务端的唯一入口状态机（src/session/http 用到，
 * 实现在 src/protocol/http/request_reader.c）。它已经出过一次线上问题——
 * 响应头缓冲不足导致 HLS 卡死。而请求侧的分片/keep-alive 行为此前没有测试：
 *   - 一次 TCP 到达半个请求 → 必须等齐再回调，不能提前 emit
 *   - 一次到达 1.5 个请求 → 回调 2 次，不能丢第二个
 *   - Content-Length 与 body 分两段到达 → body 必须拼完整
 *   - 超大 Content-Length → 拒绝，不得无限缓冲
 */
#include "zcs/protocol/http/request_reader.h"

#include "zcs_test.h"


typedef struct capture {
    int count;
    char method[16];
    char path[512];
    char host[128];
    char body[256];
    size_t body_len;
    int ws;
} capture;

static void on_request(const zcs_http_request *req, void *user)
{
    capture *c = (capture *)user;

    ++c->count;
    snprintf(c->method, sizeof(c->method), "%s", req->method);
    snprintf(c->path, sizeof(c->path), "%s", req->path);
    snprintf(c->host, sizeof(c->host), "%s", zcs_http_req_header(req, "Host"));
    c->body_len = req->body_len;
    if (req->body && req->body_len < sizeof(c->body)) {
        memcpy(c->body, req->body, req->body_len);
        c->body[req->body_len] = '\0';
    }
    c->ws = zcs_http_req_is_ws_upgrade(req);
}

/* 1) 单次完整请求 */
static int test_single_request(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *req = "GET /live/s1.m3u8 HTTP/1.1\r\nHost: a.b\r\nUser-Agent: t\r\n\r\n";

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);
    CHECK(zcs_http_request_reader_input(r, req, strlen(req)) == ZN_OK);
    CHECK(c.count == 1);
    CHECK(strcmp(c.method, "GET") == 0);
    CHECK(strcmp(c.path, "/live/s1.m3u8") == 0);
    CHECK(strcmp(c.host, "a.b") == 0);
    CHECK(c.body_len == 0);
    zcs_http_request_reader_destroy(r);
    return 0;
}

/* 2) 逐字节分片：最后一个字节到达前不得回调 */
static int test_byte_by_byte(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *req = "GET /vod/a.mp4 HTTP/1.1\r\nHost: h\r\n\r\n";
    size_t i;
    size_t n = strlen(req);

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);
    for (i = 0; i < n; ++i) {
        CHECK(zcs_http_request_reader_input(r, req + i, 1) == ZN_OK);
        if (i + 1 < n) {
            CHECK(c.count == 0); /* 未收齐不得提前回调 */
        }
    }
    CHECK(c.count == 1);
    CHECK(strcmp(c.path, "/vod/a.mp4") == 0);
    zcs_http_request_reader_destroy(r);
    return 0;
}

/* 3) 一次到达 1.5 个请求：应回调 1 次，剩余半包等后续 */
static int test_pipelined(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *a = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";
    const char *b_full = "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";
    char buf[256];

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);

    snprintf(buf, sizeof(buf), "%s%s", a, b_full);
    /* 只送到 b 的 "GET /b HTT" 处 */
    {
        size_t cut = strlen(a) + 10;

        CHECK(zcs_http_request_reader_input(r, buf, cut) == ZN_OK);
        CHECK(c.count == 1);
        CHECK(strcmp(c.path, "/a") == 0);
        /* 补完第二个请求 */
        CHECK(zcs_http_request_reader_input(r, buf + cut, strlen(buf) - cut) == ZN_OK);
        CHECK(c.count == 2);
        CHECK(strcmp(c.path, "/b") == 0);
    }
    zcs_http_request_reader_destroy(r);
    return 0;
}

/* 4) POST + Content-Length：body 分两段到达 */
static int test_content_length_body(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *head = "POST /api HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\n";
    const char *body1 = "hello ";
    const char *body2 = "world";

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);

    CHECK(zcs_http_request_reader_input(r, head, strlen(head)) == ZN_OK);
    CHECK(c.count == 0); /* body 未到齐，不回调 */
    CHECK(zcs_http_request_reader_input(r, body1, strlen(body1)) == ZN_OK);
    CHECK(c.count == 0);
    CHECK(zcs_http_request_reader_input(r, body2, strlen(body2)) == ZN_OK);
    CHECK(c.count == 1);
    CHECK(c.body_len == 11);
    CHECK(strcmp(c.body, "hello world") == 0);
    CHECK(strcmp(c.method, "POST") == 0);
    zcs_http_request_reader_destroy(r);
    return 0;
}

/* 5) 超大 Content-Length：必须拒绝，不得继续缓冲 */
static int test_oversized_body_rejected(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *req =
        "POST /api HTTP/1.1\r\nHost: h\r\nContent-Length: 999999\r\n\r\n";

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);
    (void)zcs_http_request_reader_input(r, req, strlen(req));
    CHECK(c.count == 0); /* 不得回调（读取器已拒绝） */
    zcs_http_request_reader_destroy(r);
    return 0;
}

/* 6) WebSocket 升级判定 */
static int test_ws_upgrade(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *ws =
        "GET /ws HTTP/1.1\r\nHost: h\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);
    CHECK(zcs_http_request_reader_input(r, ws, strlen(ws)) == ZN_OK);
    CHECK(c.count == 1);
    CHECK(c.ws == 1); /* Upgrade 比较应大小写不敏感 */
    zcs_http_request_reader_destroy(r);
    return 0;
}

/* 7) reset 后应能干净地重新开始（keep-alive 复用路径） */
static int test_reset_between_requests(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *req = "GET /x HTTP/1.1\r\nHost: h\r\n\r\n";

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);
    CHECK(zcs_http_request_reader_input(r, req, strlen(req)) == ZN_OK);
    CHECK(c.count == 1);
    zcs_http_request_reader_reset(r);
    CHECK(zcs_http_request_reader_input(r, req, strlen(req)) == ZN_OK);
    CHECK(c.count == 2);
    zcs_http_request_reader_destroy(r);
    return 0;
}

/* 8) 非法方法/垃圾数据不得崩溃 */
static int test_garbage_input(void)
{
    zcs_http_request_reader_opts opts = {on_request, NULL};
    zcs_http_request_reader *r;
    capture c;
    const char *junk = "\x00\x01\x02 not an http request\r\n\r\n";

    memset(&c, 0, sizeof(c));
    opts.user = &c;
    r = zcs_http_request_reader_create(&opts);
    CHECK(r != NULL);
    (void)zcs_http_request_reader_input(r, junk, strlen(junk));
    /* 不要求具体行为，只要不崩、不误回调 */
    CHECK(c.count == 0);
    zcs_http_request_reader_destroy(r);
    return 0;
}

int main(void)
{
    if (test_single_request()) {
        return 1;
    }
    if (test_byte_by_byte()) {
        return 1;
    }
    if (test_pipelined()) {
        return 1;
    }
    if (test_content_length_body()) {
        return 1;
    }
    if (test_oversized_body_rejected()) {
        return 1;
    }
    if (test_ws_upgrade()) {
        return 1;
    }
    if (test_reset_between_requests()) {
        return 1;
    }
    if (test_garbage_input()) {
        return 1;
    }
    printf("test_http_request_reader: OK\n");
    return 0;
}
