/**
 * @file test_device_core.c
 * @brief 设备/通道/能力注册表：配置通道、在线同步、发现通道清理。
 */
#include "zms/core/gop/queue.h"
#include "zms/core/stream/hub.h"
#include "zms/device/core/device_core.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define TEST_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define TEST_MKDIR(p) mkdir((p), 0777)
#endif

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    zms_device_channel_info ch;
    zms_media_source *src;

    zms_device_core_init();
    zms_media_source_registry_init();

    memset(&ch, 0, sizeof(ch));
    snprintf(ch.id, sizeof(ch.id), "34020000001320000001");
    snprintf(ch.gb_id, sizeof(ch.gb_id), "34020000001320000001");
    snprintf(ch.name, sizeof(ch.name), "Front Door");
    snprintf(ch.vhost, sizeof(ch.vhost), "__defaultVhost__");
    snprintf(ch.app, sizeof(ch.app), "live");
    snprintf(ch.stream, sizeof(ch.stream), "cam1");
    ch.flags = ZMS_DEV_CH_GB;
    ch.caps = ZMS_DEV_CAP_LIVE | ZMS_DEV_CAP_RECORD_QUERY;
    CHECK(zms_device_core_register(&ch) == 0);
    CHECK(zms_device_core_count(0, ZMS_DEV_CH_GB) == 1);

    memset(&ch, 0, sizeof(ch));
    CHECK(zms_device_core_get("34020000001320000001", &ch) == 0);
    CHECK(strcmp(ch.name, "Front Door") == 0);
    CHECK((ch.flags & ZMS_DEV_CH_CONFIGURED) != 0);
    CHECK(ch.online == 0);

    CHECK(zms_device_core_get_key("", "live", "cam1", &ch) == 0);
    CHECK(strcmp(ch.id, "34020000001320000001") == 0);

    /* 无在线流时 sync 只刷新离线状态 */
    CHECK(zms_device_core_sync_live() >= 1);
    CHECK(zms_device_core_get("34020000001320000001", &ch) == 0);
    CHECK(ch.online == 0);

    /* 制造一个在线流：sync 应生成 DISCOVERED|ONVIF 条目 */
    src = zms_media_source_register("rtmp", "live", "cam2");
    CHECK(src != NULL);
    if (!src->gop_queue) {
        src->gop_queue = zms_gop_queue_create();
    }
    CHECK(src->gop_queue != NULL);
    src->publishing = 1;
    zms_device_core_sync_live();
    memset(&ch, 0, sizeof(ch));
    CHECK(zms_device_core_get_key("", "live", "cam2", &ch) == 0);
    CHECK((ch.flags & ZMS_DEV_CH_DISCOVERED) != 0);
    CHECK((ch.flags & ZMS_DEV_CH_ONVIF) != 0);
    CHECK((ch.caps & ZMS_DEV_CAP_LIVE) != 0);
    CHECK(ch.online == 1);

    /* 配置通道仍保留，且可被 ONVIF 看见 */
    memset(&ch, 0, sizeof(ch));
    CHECK(zms_device_core_get("34020000001320000001", &ch) == 0);
    CHECK((ch.flags & ZMS_DEV_CH_GB) != 0);

    /* 流下线后 DISCOVERED 条目被清理 */
    src->publishing = 0;
    zms_device_core_sync_live();
    CHECK(zms_device_core_get_key("", "live", "cam2", &ch) != 0);

    /* 录像索引：文件名即起录时间 */
    {
        zms_device_record_list *rl;
        zms_device_record_entry re;
        int64_t off = 0;
        FILE *fp;

        zms_device_core_set_record_root("device_core_record_tmp");
        (void)TEST_MKDIR("device_core_record_tmp");
        (void)TEST_MKDIR("device_core_record_tmp/cam1");
        (void)TEST_MKDIR("device_core_record_tmp/cam1/2026-11-09");
        fp = fopen("device_core_record_tmp/cam1/2026-11-09/20261109100000.mp4", "wb");
        CHECK(fp != NULL);
        fputs("stub", fp);
        fclose(fp);

        rl = zms_device_record_query("34020000001320000001", "2026-11-09T10:00:00",
                                     "2026-11-09T10:00:10");
        CHECK(rl != NULL);
        CHECK(zms_device_record_list_count(rl) == 1);
        CHECK(strcmp(zms_device_record_list_at(rl, 0)->channel_id, "34020000001320000001") == 0);
        zms_device_record_list_free(rl);
        CHECK(zms_device_record_pick("34020000001320000001", "2026-11-09T10:00:02",
                                     "2026-11-09T10:00:08", &re, &off) == 0);
        CHECK(off == 2000);
        remove("device_core_record_tmp/cam1/2026-11-09/20261109100000.mp4");
        zms_device_core_set_record_root("");
    }

    /* 配置通道删除 */
    CHECK(zms_device_core_unregister("34020000001320000001") == 0);
    CHECK(zms_device_core_get("34020000001320000001", &ch) != 0);
    CHECK(zms_device_core_count(0, ZMS_DEV_CH_GB) == 0);

    zms_device_core_fini();
    printf("test_device_core ok\n");
    return 0;
}
