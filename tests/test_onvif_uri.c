/**
 * @file test_onvif_uri.c
 * @brief ONVIF 取流地址注入凭据（zms_onvif_uri_with_auth）回归。
 */
#include "zms/device/onvif/onvif.h"
#include <stdio.h>
#include <string.h>

static int check(const char *in, const char *u, const char *p, const char *want)
{
    char out[512];
    if (zms_onvif_uri_with_auth(in, u, p, out, sizeof(out)) != 0) {
        printf("FAIL rc in=%s\n", in);
        return 1;
    }
    if (strcmp(out, want) != 0) {
        printf("MISMATCH in=%s\n  got =%s\n  want=%s\n", in, out, want);
        return 1;
    }
    return 0;
}

int main(void)
{
    int fails = 0;

    /* 带查询参数的 RTSP 地址（海康等设备返回此形式）：必须保留 host:port */
    fails += check(
        "rtsp://192.0.2.10:554/Streaming/Channels/102?transportmode=unicast&profile=Profile_2",
        "admin", "secret",
        "rtsp://admin:secret@192.0.2.10:554/Streaming/Channels/102?transportmode=unicast&profile=Profile_2");
    fails += check("rtsp://10.0.0.1/live/main", "u", "p", "rtsp://u:p@10.0.0.1/live/main");
    /* 已带 userinfo / 无凭据：原样输出 */
    fails += check("rtsp://a:b@10.0.0.1/live", "u", "p", "rtsp://a:b@10.0.0.1/live");
    fails += check("rtsp://10.0.0.1/live", NULL, NULL, "rtsp://10.0.0.1/live");
    /* 特殊字符需要 URL 编码 */
    fails += check("rtsp://h:554/s", "u@1", "p:a/1", "rtsp://u%401:p%3Aa%2F1@h:554/s");

    if (fails) {
        printf("test_onvif_uri FAIL (%d)\n", fails);
        return 1;
    }
    printf("test_onvif_uri ok\n");
    return 0;
}
