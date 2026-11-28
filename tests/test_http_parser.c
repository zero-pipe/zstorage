/**
 * @file test_http_parser.c
 * @brief HTTP/RTSP 报文解析：头字段按需堆分配、复用与释放。
 *
 * 回归点：头字段 value 从定长数组改成堆指针后，拷贝路径必须一起改，
 * 否则会把 memcpy 写进未初始化指针（曾在压测首包就 segfault）。
 */
#include "zcs/protocol/http/parser.h"
#include "zcs/protocol/rtsp/parser.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int test_http_request(void)
{
    static zcs_http_parser p;
    const char *req = "GET /live/s1.live.flv?token=abc HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Range: bytes=0-\r\n"
                      "\r\n";

    zcs_http_parser_init(&p);
    CHECK(zcs_http_parser_parse(&p, req, strlen(req)) == ZN_OK);
    CHECK(strcmp(p.method, "GET") == 0);
    CHECK(strcmp(p.url, "/live/s1.live.flv") == 0);
    CHECK(strcmp(p.params, "token=abc") == 0);
    CHECK(p.header_count == 2);
    CHECK(strcmp(zcs_http_parser_header(&p, "Host"), "127.0.0.1") == 0);
    CHECK(strcmp(zcs_http_parser_header(&p, "Missing"), "") == 0);

    /* 复用同一个 parser：旧的堆字段必须先释放再覆盖 */
    CHECK(zcs_http_parser_parse(&p, req, strlen(req)) == ZN_OK);
    CHECK(p.header_count == 2);
    zcs_http_parser_clear(&p);
    zcs_http_parser_clear(&p); /* 幂等 */
    return 0;
}

static int test_http_long_header(void)
{
    static zcs_http_parser p;
    char req[4096];
    char big[2000];
    int n;

    memset(big, 'a', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    n = snprintf(req, sizeof(req),
                 "POST /index/api/x HTTP/1.1\r\nHost: h\r\nCookie: %s\r\n\r\n", big);
    CHECK(n > 0 && (size_t)n < sizeof(req));

    zcs_http_parser_init(&p);
    CHECK(zcs_http_parser_parse(&p, req, (size_t)n) == ZN_OK);
    CHECK(strlen(zcs_http_parser_header(&p, "Cookie")) == sizeof(big) - 1);
    zcs_http_parser_clear(&p);
    return 0;
}

static int test_rtsp_message(void)
{
    static zcs_rtsp_message m;
    const char *msg = "OPTIONS rtsp://h/live/s1 RTSP/1.0\r\n"
                      "CSeq: 1\r\n"
                      "Session: 12345678\r\n"
                      "\r\n";

    memset(&m, 0, sizeof(m));
    CHECK(zcs_rtsp_message_parse_header(msg, strlen(msg), &m) == ZN_OK);
    CHECK(m.msg.hdr_n == 2);
    CHECK(strcmp(zcs_rtsp_message_get(&m, "CSeq"), "1") == 0);
    CHECK(strcmp(zcs_rtsp_message_get(&m, "Session"), "12345678") == 0);
    CHECK(strcmp(zcs_rtsp_message_url(&m), "rtsp://h/live/s1") == 0);
    CHECK(!zcs_rtsp_message_is_response(&m));
    /* 复用：clear 内部释放旧头字段 */
    CHECK(zcs_rtsp_message_parse_header(msg, strlen(msg), &m) == ZN_OK);
    CHECK(m.msg.hdr_n == 2);
    zcs_rtsp_message_clear(&m);
    zcs_rtsp_message_clear(&m);

    /* 响应消息：状态码/原因，body 带 Content-Length */
    {
        const char *resp = "RTSP/1.0 200 OK\r\nCSeq: 7\r\nContent-Length: 5\r\n\r\nhello";
        size_t consumed = 0;

        CHECK(zcs_rtsp_message_parse(resp, strlen(resp), &m, &consumed) == ZN_OK);
        CHECK(consumed == strlen(resp));
        CHECK(zcs_rtsp_message_is_response(&m));
        CHECK(m.status_code == 200);
        CHECK(strcmp(m.msg.reason, "OK") == 0);
        CHECK(zcs_rtsp_message_body_len(&m) == 5);
        CHECK(memcmp(zcs_rtsp_message_body(&m), "hello", 5) == 0);
        zcs_rtsp_message_clear(&m);
    }
    return 0;
}

int main(void)
{
    int r;

    if ((r = test_http_request()) != 0) {
        return r;
    }
    if ((r = test_http_long_header()) != 0) {
        return r;
    }
    if ((r = test_rtsp_message()) != 0) {
        return r;
    }
    printf("test_http_parser: ok\n");
    return 0;
}
