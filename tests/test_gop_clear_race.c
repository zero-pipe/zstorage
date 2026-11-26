/**
 * @file test_gop_clear_race.c
 * @brief 回归：清流（推流重连/linger 过期/webhook close）与并发播放读者、
 *        并发写者的竞争。历史上这里会在读者无锁临界区里立即释放 slot 缓冲，
 *        造成 WHEP / WS-FLV / RTSP 播放线程随机段错误。
 *
 * 本测试让三条线程同时跑：写者持续 push、读者持续 read、主线程反复 clear，
 * 只要不崩溃、读者在 clear 后仍能读到新帧即通过。
 */
#include "zms/core/gop/queue.h"
#include "zms/core/frame.h"
#include "zb/os/thread.h"
#include "zn/platform.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static zms_gop_queue *g_q;
static volatile int g_stop;
static volatile long g_reads;
static volatile long g_writes;

static void writer_fn(void *user)
{
    static uint8_t frame[700];
    uint64_t dts = 0;
    int i = 0;

    (void)user;
    memset(frame, 0x5a, sizeof(frame));
    while (!g_stop) {
        zms_frame f;

        memset(&f, 0, sizeof(f));
        f.data = frame;
        f.size = sizeof(frame);
        f.dts_ms = f.pts_ms = dts;
        f.codec = ZMS_CODEC_H264;
        f.track = ZMS_TRACK_VIDEO;
        f.keyframe = ((i % 10) == 0);
        if (zms_gop_queue_write(g_q, &f) == ZN_OK) {
            g_writes++;
        }
        dts += 33;
        ++i;
    }
}

static void reader_fn(void *user)
{
    zms_gop_reader *rd = (zms_gop_reader *)user;

    while (!g_stop) {
        zms_gop_slot slot;
        int n = zms_gop_reader_read(rd, &slot);

        if (n > 0) {
            /* 真正触碰数据，确保读到的是有效内存而不是被回收的 buf */
            volatile uint8_t sink = slot.data[0];
            (void)sink;
            g_reads++;
        }
    }
}

int main(void)
{
    zms_gop_reader *rd;
    zb_thread *tw;
    zb_thread *tr;

    g_q = zms_gop_queue_create();
    CHECK(g_q != NULL);
    zms_gop_queue_set_cache_ms(g_q, 0);
    zms_gop_queue_set_byte_budget(g_q, 0);

    rd = zms_gop_reader_attach(g_q);
    CHECK(rd != NULL);

    g_stop = 0;
    tw = zb_thread_create(writer_fn, NULL);
    tr = zb_thread_create(reader_fn, rd);
    CHECK(tw != NULL && tr != NULL);

    /* 反复清流，制造“clear 与读者/写者并发”的窗口 */
    for (int i = 0; i < 200; ++i) {
        zn_sleep_ms(2);
        zms_gop_queue_clear(g_q);
    }

    g_stop = 1;
    zb_thread_join(tw);
    zb_thread_join(tr);
    zb_thread_destroy(tw);
    zb_thread_destroy(tr);

    CHECK(g_writes > 0);
    /* 读者在反复 clear 下也应读到相当数量的帧（不冻结） */
    CHECK(g_reads > 0);

    zms_gop_reader_detach(rd);
    zms_gop_queue_destroy(g_q);
    printf("test_gop_clear_race: ok (reads=%ld writes=%ld)\n", g_reads, g_writes);
    return 0;
}
