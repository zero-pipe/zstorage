/**
 * @file test_gop_trim.c
 * @brief GOP 缓存回收：时间窗裁剪、字节上限裁剪、字节记账与清空。
 */
#include "zms/core/gop/queue.h"
#include "zms/core/frame.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* 构造一个"视频帧"：H.264 Annex-B 形态，keyframe 控制 GOP 边界。 */
static void push_video(zms_gop_queue *q, uint64_t dts_ms, int keyframe, const uint8_t *payload,
                       size_t len)
{
    zms_frame f;

    memset(&f, 0, sizeof(f));
    f.data = (uint8_t *)payload;
    f.size = len;
    f.dts_ms = dts_ms;
    f.pts_ms = dts_ms;
    f.codec = ZMS_CODEC_H264;
    f.track = ZMS_TRACK_VIDEO;
    f.keyframe = keyframe ? 1 : 0;
    (void)zms_gop_queue_write(q, &f);
}

static size_t test_time_window(void)

{
    static uint8_t frame[1000];
    zms_gop_queue *q = zms_gop_queue_create();
    size_t bytes;

    CHECK(q != NULL);
    memset(frame, 0x42, sizeof(frame));

    zms_gop_queue_set_cache_ms(q, 1000); /* 1 秒窗口 */
    zms_gop_queue_set_byte_budget(q, 0); /* 只测时间 */

    /* 10fps、每 10 帧一个关键帧（GOP=1s），共 6 秒 */
    for (int i = 0; i < 60; ++i) {
        push_video(q, (uint64_t)i * 100u, (i % 10) == 0, frame, sizeof(frame));
    }
    bytes = zms_gop_queue_bytes(q);
    /* 窗口 1s + 当前 GOP：最多 ~2~3 个 GOP，绝不是 60 帧全留 */
    CHECK(zms_gop_queue_gop_count(q) <= 3);
    CHECK(zms_gop_queue_gop_count(q) >= 1);
    CHECK(zms_gop_queue_pending_count(q) <= 40);
    CHECK(bytes > 0 && bytes <= 40u * sizeof(frame));
    zms_gop_queue_destroy(q);
    return 0;
}

static int test_byte_budget(void)
{
    static uint8_t frame[1000];
    zms_gop_queue *q = zms_gop_queue_create();
    size_t bytes;

    CHECK(q != NULL);
    memset(frame, 0x11, sizeof(frame));

    zms_gop_queue_set_cache_ms(q, 0);              /* 关时间窗 */
    zms_gop_queue_set_byte_budget(q, 4u * 1024u);  /* 4KB 上限 */

    for (int i = 0; i < 60; ++i) {
        push_video(q, (uint64_t)i * 40u, (i % 10) == 0, frame, sizeof(frame));
    }
    bytes = zms_gop_queue_bytes(q);
    /* 至少保留当前 GOP（10 帧 = 10KB），但不会保留全部 60KB */
    CHECK(bytes >= 1);
    CHECK(bytes < 60u * sizeof(frame));
    CHECK(zms_gop_queue_gop_count(q) >= 1);
    zms_gop_queue_destroy(q);
    return 0;
}

static int test_bytes_accounting(void)
{
    static uint8_t frame[500];
    zms_gop_queue *q = zms_gop_queue_create();
    size_t before;
    size_t after;

    CHECK(q != NULL);
    memset(frame, 0x22, sizeof(frame));
    zms_gop_queue_set_cache_ms(q, 0);
    zms_gop_queue_set_byte_budget(q, 0);

    CHECK(zms_gop_queue_bytes(q) == 0);
    push_video(q, 0, 1, frame, sizeof(frame));
    before = zms_gop_queue_bytes(q);
    CHECK(before == sizeof(frame));
    push_video(q, 40, 0, frame, sizeof(frame));
    after = zms_gop_queue_bytes(q);
    CHECK(after == 2u * sizeof(frame));

    /* clear 后记账归零 */
    zms_gop_queue_clear(q);
    CHECK(zms_gop_queue_bytes(q) == 0);
    CHECK(zms_gop_queue_pending_count(q) == 0);
    zms_gop_queue_destroy(q);
    return 0;
}

/*
 * 清流不冻结已挂读者：
 *   读者先消费一部分，随后 clear（推流重连/linger 过期会触发），再写入新帧，
 *   读者应能继续读到新帧（write 游标不归零、oldest_seq 前移），且不崩溃。
 */
static int test_clear_with_live_reader(void)
{
    static uint8_t frame[300];
    zms_gop_queue *q = zms_gop_queue_create();
    zms_gop_reader *rd;
    zms_gop_slot slot;
    int got;

    CHECK(q != NULL);
    memset(frame, 0x33, sizeof(frame));
    zms_gop_queue_set_cache_ms(q, 0);
    zms_gop_queue_set_byte_budget(q, 0);

    push_video(q, 0, 1, frame, sizeof(frame));
    push_video(q, 40, 0, frame, sizeof(frame));

    rd = zms_gop_reader_attach(q);
    CHECK(rd != NULL);
    got = 0;
    while (zms_gop_reader_read(rd, &slot) > 0) {
        ++got;
    }
    CHECK(got == 2);

    /* 模拟 source_clear：读者仍挂着时清空队列 */
    zms_gop_queue_clear(q);
    CHECK(zms_gop_queue_pending_count(q) == 0);

    /* 清流后新码流到达：读者必须能读到，而不是永久返回 0 */
    push_video(q, 80, 1, frame, sizeof(frame));
    push_video(q, 120, 0, frame, sizeof(frame));
    got = 0;
    while (zms_gop_reader_read(rd, &slot) > 0) {
        ++got;
    }
    CHECK(got == 2);

    zms_gop_reader_detach(rd);
    zms_gop_queue_destroy(q);
    return 0;
}

/* 视频 config 的借用指针在 clear / 重新设置后仍指向队列内缓冲（不再悬空）。 */
static int test_video_config_pointer_stable(void)
{
    static uint8_t cfg1[16];
    static uint8_t cfg2[16];
    static uint8_t frame[64];
    zms_gop_queue *q = zms_gop_queue_create();
    const uint8_t *p1;
    const uint8_t *p2;
    size_t len1 = 0;

    CHECK(q != NULL);
    memset(cfg1, 0x01, sizeof(cfg1));
    memset(cfg2, 0x02, sizeof(cfg2));
    memset(frame, 0x44, sizeof(frame));

    CHECK(zms_gop_queue_set_video_config(q, cfg1, sizeof(cfg1)) == ZN_OK);
    p1 = zms_gop_queue_video_config(q, &len1);
    CHECK(p1 != NULL && len1 == sizeof(cfg1));
    CHECK(p1[0] == 0x01);

    /* 覆盖配置 + 清流后，原指针所属内存不能被释放（仍可安全读取） */
    CHECK(zms_gop_queue_set_video_config(q, cfg2, sizeof(cfg2)) == ZN_OK);
    zms_gop_queue_clear(q);
    CHECK(p1[0] == 0x01); /* 指针有效（内容可能是旧一代，但不崩溃） */

    p2 = zms_gop_queue_video_config(q, &len1);
    CHECK(p2 == NULL && len1 == 0); /* clear 后没有视频配置 */

    zms_gop_queue_destroy(q);
    return 0;
}

int main(void)
{
    int r;

    if ((r = (int)test_time_window()) != 0) {
        return r;
    }
    if ((r = test_byte_budget()) != 0) {
        return r;
    }
    if ((r = test_bytes_accounting()) != 0) {
        return r;
    }
    if ((r = test_clear_with_live_reader()) != 0) {
        return r;
    }
    if ((r = test_video_config_pointer_stable()) != 0) {
        return r;
    }
    printf("test_gop_trim: ok\n");
    return 0;
}
