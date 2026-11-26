/**
 * @file test_proto_xml.c
 * @brief 协议层 XML 文档助手 + MANSCDP 按标签取值。
 */
#include "zms/message/msg.h"
#include "zms/message/xml.h"
#include "device/gb28181/gb28181_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int test_msg_xml(void)
{
    static zms_msg m;
    const char *xml = "<?xml version=\"1.0\"?><Query><CmdType>Catalog</CmdType>"
                      "<SN>12345</SN><DeviceID>34020000001320000001</DeviceID></Query>";
    char buf[64];
    int n = 0;
    static const char *roots[] = {"Notify", "Response", "Query", "Control"};
    char root[16];

    zms_msg_init(&m);
    CHECK(zms_msg_set_body(&m, xml, strlen(xml)) == 0);

    CHECK(zms_msg_xml_get(&m, "CmdType", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Catalog") == 0);
    CHECK(zms_msg_xml_get(&m, "DeviceID", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "34020000001320000001") == 0);
    CHECK(zms_msg_xml_get_int(&m, "SN", 0, &n) == 0 && n == 12345);
    CHECK(zms_msg_xml_root(&m, roots, 4, root, sizeof(root)) == 0);
    CHECK(strcmp(root, "Query") == 0);
    /* 不存在的标签：返回 -1 且清空 */
    CHECK(zms_msg_xml_get(&m, "NotThere", buf, sizeof(buf)) == -1 && buf[0] == '\0');
    /* 无 body */
    zms_msg_init(&m);
    CHECK(zms_msg_xml_get(&m, "CmdType", buf, sizeof(buf)) == -1);
    return 0;
}

static int test_manscdp(void)
{
    gb_manscdp_msg q;
    const char *body = "<Control><CmdType>DeviceControl</CmdType><SN>7</SN>"
                       "<DeviceID>34020000001320000001</DeviceID>"
                       "<PTZCmd>A50F0102000000</PTZCmd>"
                       "<SomeFutureTag>hello</SomeFutureTag></Control>";
    char buf[64];

    CHECK(gb_manscdp_parse(body, strlen(body), &q) == 0);
    CHECK(strcmp(q.root, "Control") == 0);
    CHECK(strcmp(q.cmd_type, "DeviceControl") == 0);
    CHECK(strcmp(q.device_id, "34020000001320000001") == 0);
    CHECK(strcmp(q.ptz_cmd, "A50F0102000000") == 0);
    /* 结构体里没有的字段：按标签名取，不需要扩展结构 */
    CHECK(gb_manscdp_get(&q, "SomeFutureTag", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "hello") == 0);
    CHECK(gb_manscdp_get(&q, "Missing", buf, sizeof(buf)) == -1);
    return 0;
}

int main(void)
{
    int r;

    if ((r = test_msg_xml()) != 0) {
        return r;
    }
    if ((r = test_manscdp()) != 0) {
        return r;
    }
    printf("test_proto_xml: ok\n");
    return 0;
}
