/**
 * @file test_dns_cache.c
 * @brief 解析缓存：IP 字面量直通、失败回退、缓存命中、预热与清空。
 */
#include "zms/util/dns_cache.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    char ip[64];
    const char *t;
    char buf[64];

    zms_dns_cache_flush();

    /* IP 字面量：原样回填，不需要解析 */
    CHECK(zms_dns_resolve("10.1.2.3", ip, sizeof(ip)) == 0);
    CHECK(strcmp(ip, "10.1.2.3") == 0);
    CHECK(zms_dns_resolve("::1", ip, sizeof(ip)) == 0);
    CHECK(strcmp(ip, "::1") == 0);

    /* 非法参数 */
    CHECK(zms_dns_resolve(NULL, ip, sizeof(ip)) == -1);
    CHECK(zms_dns_resolve("", ip, sizeof(ip)) == -1);
    CHECK(zms_dns_resolve("x", NULL, 0) == -1);

    /* 解析失败的域名：target 原样返回 hostama，进程不崩 */
    t = zms_dns_target("no-such-host.invalid", buf, sizeof(buf));
    CHECK(t != NULL && strcmp(t, "no-such-host.invalid") == 0);
    CHECK(zms_dns_resolve("no-such-host.invalid", ip, sizeof(ip)) == -1);
    CHECK(ip[0] == '\0');

    /* 本地回环：能解析则缓存，第二次命中；不能解析也不判失败（容器环境差异） */
    if (zms_dns_resolve("localhost", ip, sizeof(ip)) == 0) {
        char again[64];

        CHECK(ip[0] != '\0');
        CHECK(zms_dns_resolve("localhost", again, sizeof(again)) == 0);
        CHECK(strcmp(ip, again) == 0);
    }

    /* 预热 URL 形态：不崩即可（解析结果取决于环境） */
    zms_dns_cache_warm("http://127.0.0.1:8080/live/s1.flv");
    zms_dns_cache_warm("rtsp://user:pw@127.0.0.1:554/live/s1");
    zms_dns_cache_warm("rtmp://[::1]/live/s1");
    zms_dns_cache_warm(NULL);
    zms_dns_cache_warm("");

    /* 清空后仍可继续使用 */
    zms_dns_cache_flush();
    CHECK(zms_dns_resolve("10.9.9.9", ip, sizeof(ip)) == 0);
    printf("test_dns_cache: ok\n");
    return 0;
}
