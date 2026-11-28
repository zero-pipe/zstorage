/**
 * @file test_device_xml.c
 * @brief 共享 XML 层与命令表：前缀无关查找、实体、属性、Action 提取、能力门控。
 */
#include "zcs/device/core/device_command.h"
#include "zcs/device/core/device_core.h"
#include "zcs/device/core/device_xml.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int exec_noop(const zcs_device_cmd_ctx *ctx)
{
    (void)ctx;
    return 0;
}

int main(void)
{
    char val[128];
    const char *xml = "<s:Envelope><s:Body><trt:GetProfiles>"
                      "<trt:ProfileToken>tok-1</trt:ProfileToken>"
                      "<tt:Name>a &amp; b</tt:Name>"
                      "<trt:Profiles token=\"prof-9\"/>"
                      "</trt:GetProfiles></s:Body></s:Envelope>";
    const char *tag;
    zcs_device_channel_info ch;

    CHECK(zcs_xml_text(xml, "ProfileToken", val, sizeof(val)) == 0);
    CHECK(strcmp(val, "tok-1") == 0);
    CHECK(zcs_xml_text(xml, "Name", val, sizeof(val)) == 0);
    CHECK(strcmp(val, "a & b") == 0);
    CHECK(zcs_xml_attr(xml, "Profiles", "token", val, sizeof(val)) == 0);
    CHECK(strcmp(val, "prof-9") == 0);
    CHECK(zcs_xml_first_child_name(xml, "Body", val, sizeof(val)) == 0);
    CHECK(strcmp(val, "GetProfiles") == 0);
    tag = zcs_xml_find_tag_start(xml, NULL, "GetProfiles");
    CHECK(tag != NULL);
    CHECK(zcs_xml_local_name(tag, val, sizeof(val)) == 0);
    CHECK(strcmp(val, "GetProfiles") == 0);
    CHECK(zcs_xml_text(xml, "Missing", val, sizeof(val)) != 0);

    {
        static const zcs_device_cmd cmds[] = {
            {"GetProfiles", "action", 0, exec_noop},
            {"PTZ", "action", ZCS_DEV_CAP_PTZ, exec_noop},
        };
        const zcs_device_cmd *cmd;

        zcs_device_core_init();
        cmd = zcs_device_cmd_find(cmds, sizeof(cmds) / sizeof(cmds[0]), "getprofiles");
        CHECK(cmd == &cmds[0]);
        CHECK(zcs_device_cmd_allowed(cmd));
        cmd = zcs_device_cmd_find(cmds, sizeof(cmds) / sizeof(cmds[0]), "PTZ");
        CHECK(cmd == &cmds[1]);
        CHECK(!zcs_device_cmd_allowed(cmd));

        memset(&ch, 0, sizeof(ch));
        snprintf(ch.id, sizeof(ch.id), "ch1");
        snprintf(ch.app, sizeof(ch.app), "live");
        snprintf(ch.stream, sizeof(ch.stream), "cam1");
        ch.flags = ZCS_DEV_CH_GB;
        ch.caps = ZCS_DEV_CAP_PTZ;
        CHECK(zcs_device_core_register(&ch) == 0);
        CHECK(zcs_device_core_any_cap(ZCS_DEV_CAP_PTZ));
        CHECK(zcs_device_cmd_allowed(cmd));
        CHECK(zcs_device_core_unregister("ch1") == 0);
        zcs_device_core_fini();
    }

    printf("test_device_xml ok\n");
    return 0;
}
