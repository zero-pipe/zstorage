/**
 * @file test_amf_args.c
 * @brief AMF 命令参数列表：顺序取值、对象字段取值、越界与类型不符。
 */
#include "zms/media/wire/amf.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int test_play_args(void)
{
    uint8_t buf[256];
    size_t p = 0;
    zms_amf_args *a;

    /* 模拟 play 命令体：null, "stream1", 0.0, -1.0, true */
    p += zms_amf_encode_null(buf + p, sizeof(buf) - p);
    p += zms_amf_encode_string(buf + p, sizeof(buf) - p, "stream1");
    p += zms_amf_encode_number(buf + p, sizeof(buf) - p, 0.0);
    p += zms_amf_encode_number(buf + p, sizeof(buf) - p, -1.0);
    p += zms_amf_encode_boolean(buf + p, sizeof(buf) - p, 1);

    a = zms_amf_args_parse(buf, p);
    CHECK(a != NULL);
    CHECK(zms_amf_args_count(a) == 5);
    CHECK(zms_amf_args_at(a, 0) != NULL);
    CHECK(zms_amf_value_type(zms_amf_args_at(a, 0)) == ZMS_AMF_NULL);
    CHECK(zms_amf_args_str(a, 1) && strcmp(zms_amf_args_str(a, 1), "stream1") == 0);
    CHECK(zms_amf_args_num(a, 2, -9) == 0.0);
    CHECK(zms_amf_args_num(a, 3, -9) == -1.0);
    CHECK(zms_amf_args_bool(a, 4, 0) == 1);
    /* 类型不符/越界：返回默认值，不崩 */
    CHECK(zms_amf_args_str(a, 2) == NULL);
    CHECK(zms_amf_args_num(a, 1, -9) == -9);
    CHECK(zms_amf_args_bool(a, 1, 1) == 1);
    CHECK(zms_amf_args_str(a, 99) == NULL);
    CHECK(zms_amf_args_num(a, -1, -9) == -9);
    zms_amf_args_free(a);
    return 0;
}

static int test_connect_object(void)
{
    uint8_t buf[512];
    size_t p = 0;
    zms_amf_args *a;

    /* connect 命令体：null, { app: "live", tcUrl: "rtmp://h/live", objectEncoding: 0 } */
    p += zms_amf_encode_null(buf + p, sizeof(buf) - p);
    p += zms_amf_encode_object_begin(buf + p, sizeof(buf) - p);
    p += zms_amf_encode_object_key(buf + p, sizeof(buf) - p, "app");
    p += zms_amf_encode_string(buf + p, sizeof(buf) - p, "live");
    p += zms_amf_encode_object_key(buf + p, sizeof(buf) - p, "tcUrl");
    p += zms_amf_encode_string(buf + p, sizeof(buf) - p, "rtmp://h/live");
    p += zms_amf_encode_object_key(buf + p, sizeof(buf) - p, "objectEncoding");
    p += zms_amf_encode_number(buf + p, sizeof(buf) - p, 0);
    p += zms_amf_encode_object_end(buf + p, sizeof(buf) - p);

    a = zms_amf_args_parse(buf, p);
    CHECK(a != NULL);
    CHECK(zms_amf_args_count(a) == 2);
    CHECK(strcmp(zms_amf_args_obj_str(a, 1, "app"), "live") == 0);
    CHECK(strcmp(zms_amf_args_obj_str(a, 1, "tcUrl"), "rtmp://h/live") == 0);
    CHECK(zms_amf_args_obj_num(a, 1, "objectEncoding", -1) == 0);
    CHECK(zms_amf_args_obj_str(a, 1, "missing") == NULL);
    /* 非对象参数：对象取值返回 NULL/默认值 */
    CHECK(zms_amf_args_obj_str(a, 0, "app") == NULL);
    zms_amf_args_free(a);
    return 0;
}

static int test_empty(void)
{
    CHECK(zms_amf_args_parse(NULL, 0) == NULL);
    CHECK(zms_amf_args_count(NULL) == 0);
    CHECK(zms_amf_args_at(NULL, 0) == NULL);
    CHECK(zms_amf_args_str(NULL, 0) == NULL);
    CHECK(zms_amf_args_num(NULL, 0, 7) == 7);
    zms_amf_args_free(NULL);
    return 0;
}

int main(void)
{
    int r;

    if ((r = test_play_args()) != 0) {
        return r;
    }
    if ((r = test_connect_object()) != 0) {
        return r;
    }
    if ((r = test_empty()) != 0) {
        return r;
    }
    printf("test_amf_args: ok\n");
    return 0;
}
