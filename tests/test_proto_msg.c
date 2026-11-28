/**
 * @file test_proto_msg.c
 * @brief 协议消息外壳：键值视图、大小写不敏感查找、查询串解析、上限与边界。
 */
#include "zcs/message/msg.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int test_headers(void)
{
    static zcs_msg m;
    static char line[] = "Host: 127.0.0.1";
    char buf[64];
    int64_t n = 0;

    zcs_msg_init(&m);
    CHECK(zcs_msg_add_hdr(&m, "Host", 4, "127.0.0.1", 9) == 0);
    CHECK(zcs_msg_add_hdr(&m, " Content-Length ", 16, " 42\r\n", 5) == 0);
    CHECK(zcs_msg_add_hdr(&m, "X-Empty", 7, "", 0) == 0);

    CHECK(m.hdr_n == 3);
    /* 大小写不敏感 */
    CHECK(zcs_msg_find(&m, "host") != NULL);
    CHECK(zcs_msg_find(&m, "HOST") != NULL);
    CHECK(zcs_msg_find(&m, "content-length") != NULL);
    CHECK(zcs_msg_find(&m, "missing") == NULL);
    /* 前后空白被裁掉 */
    CHECK(zcs_msg_get_copy(&m, "Content-Length", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "42") == 0);
    CHECK(zcs_msg_get_int(&m, "content-length", -1, &n) == 0 && n == 42);
    CHECK(zcs_msg_get_int(&m, "nope", -7, &n) == -1 && n == -7); /* 未找到：返回-1，写默认值 */
    /* 空值 */
    CHECK(zcs_msg_get_copy(&m, "X-Empty", buf, sizeof(buf)) == 0 && buf[0] == '\0');
    /* 未找到：out 置空串，返回 -1 */
    CHECK(zcs_msg_get_copy(&m, "nope", buf, sizeof(buf)) == -1 && buf[0] == '\0');
    /* 从可写行构造视图仍然可用（视图不持有内存） */
    CHECK(zcs_msg_add_hdr(&m, line, 4, line + 6, 9) == 0);
    CHECK(zcs_msg_get_copy(&m, "Host", buf, sizeof(buf)) == 0);
    return 0;
}

static int test_bool_and_overflow(void)
{
    static zcs_msg m;
    char small[4];

    zcs_msg_init(&m);
    CHECK(zcs_msg_add_hdr(&m, "A", 1, "TRUE", 4) == 0);
    CHECK(zcs_msg_add_hdr(&m, "B", 1, "no", 2) == 0);
    CHECK(zcs_msg_add_hdr(&m, "C", 1, "maybe", 5) == 0);
    CHECK(zcs_msg_get_bool(&m, "a", 0) == 1);
    CHECK(zcs_msg_get_bool(&m, "b", 1) == 0);
    CHECK(zcs_msg_get_bool(&m, "c", 1) == 1); /* 非布尔用默认值 */
    CHECK(zcs_msg_get_bool(&m, "missing", 1) == 1);
    /* 值比缓冲长：返回 -1 且清空 */
    CHECK(zcs_msg_get_copy(&m, "C", small, sizeof(small)) == -1 && small[0] == '\0');
    return 0;
}

static int test_header_limit(void)
{
    static zcs_msg m;
    char name[8];

    zcs_msg_init(&m);
    for (int i = 0; i < ZCS_MSG_HDR_MAX; ++i) {
        snprintf(name, sizeof(name), "H%d", i);
        CHECK(zcs_msg_add_hdr(&m, name, strlen(name), "v", 1) == 0);
    }
    CHECK(m.hdr_n == ZCS_MSG_HDR_MAX);
    CHECK(zcs_msg_add_hdr(&m, "Over", 4, "v", 1) == -1);
    return 0;
}

static int test_query(void)
{
    static zcs_msg m;
    char buf[64];
    int64_t n = 0;

    zcs_msg_init(&m);
    zcs_msg_set_url(&m, "/live/s1.live.flv?token=ab%2Bc&seek=1500&flag=yes&empty=", 55);
    CHECK(zcs_msg_query_get(&m, "token", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "ab+c") == 0);
    CHECK(zcs_msg_query_get_int(&m, "seek", 0, &n) == 0 && n == 1500);
    CHECK(zcs_msg_query_get(&m, "flag", buf, sizeof(buf)) == 0 && strcmp(buf, "yes") == 0);
    CHECK(zcs_msg_query_get(&m, "empty", buf, sizeof(buf)) == 0 && buf[0] == '\0');
    CHECK(zcs_msg_query_get(&m, "missing", buf, sizeof(buf)) == -1);
    /* '+': 解码为空格 */
    zcs_msg_set_url(&m, "/x?a=1+2", 8);
    CHECK(zcs_msg_query_get(&m, "a", buf, sizeof(buf)) == 0 && strcmp(buf, "1 2") == 0);
    /* 无查询串 */
    zcs_msg_set_url(&m, "/x", 2);
    CHECK(zcs_msg_query(&m, NULL) == NULL);
    CHECK(zcs_msg_query_get(&m, "a", buf, sizeof(buf)) == -1);
    return 0;
}

static int test_status_and_body(void)
{
    static zcs_msg m;

    zcs_msg_init(&m);
    CHECK(zcs_msg_set_status(&m, 200, "OK", 2) == 0);
    CHECK(m.kind == ZCS_MSG_KIND_RESPONSE && m.status == 200);
    CHECK(zcs_msg_set_body(&m, "abc", 3) == 0 && m.body_len == 3);
    CHECK(zcs_msg_set_body(&m, NULL, 10) == 0 && m.body_len == 0);
    return 0;
}

int main(void)
{
    int r;

    if ((r = test_headers()) != 0) {
        return r;
    }
    if ((r = test_bool_and_overflow()) != 0) {
        return r;
    }
    if ((r = test_header_limit()) != 0) {
        return r;
    }
    if ((r = test_query()) != 0) {
        return r;
    }
    if ((r = test_status_and_body()) != 0) {
        return r;
    }
    printf("test_proto_msg: ok\n");
    return 0;
}
