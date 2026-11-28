/**
 * @file test_zs_clock.c
 * @brief 录像归一化时钟回归：音视频独立时基、媒体超前墙钟限速、断流重锚。
 *
 * 为什么测它：录像索引里的 utc、片段时长、封口上报时间全部来自这一层。
 * 线上曾出现"15 分钟的槽位被记成 40 多分钟"——根因是同一条时钟被视频帧和
 * 音频帧交替喂入：两者的 dts 各自从 0 起算，跨轨差值被当成断流，时间轴越推越快。
 * 这里锁住三件事：分轨喂入不跑飞、源端 PTS 跑快时按墙钟限速、PTS 归零能重锚。
 */
#include "storage/zs_clock.h"
#include "zcs_test.h"

#include <stdlib.h>

/* 取一个真实量级的墙钟起点，避免 0 时刻触发"未锚定"的特殊分支 */
#define WALL_BASE INT64_C(1795860000000)
#define RESYNC_MS 3000
/* 允许的偏差：限速阈值 + 一帧左右的粒度误差 */
#define DRIFT_TOL_MS (RESYNC_MS + 1000)

static int64_t abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

/**
 * 15fps 视频(66ms/帧)与 16kHz AAC(64ms/帧)各自独立时基，分轨喂入。
 * 60 秒后两路 utc 都必须贴着墙钟走，且不能出现断流。
 */
static void test_independent_track_clocks(void)
{
    zs_clock *cv = zs_clock_create(NULL);
    zs_clock *ca = zs_clock_create(NULL);
    int64_t wall_v = WALL_BASE;
    int64_t wall_a = WALL_BASE;
    int64_t raw_v = 0;
    int64_t raw_a = 0;
    int64_t prev_pts_v = -1;
    int64_t prev_pts_a = -1;
    int i;

    CHECK(cv != NULL && ca != NULL);
    for (i = 0; i < 900; ++i) {
        zs_clock_out ov;
        zs_clock_out oa;

        zs_clock_input(cv, raw_v * 90, wall_v, 0, &ov);
        zs_clock_input(ca, raw_a * 90, wall_a, 0, &oa);

        CHECK(ov.valid && oa.valid);
        CHECK(ov.pts_90k >= prev_pts_v);
        CHECK(oa.pts_90k >= prev_pts_a);
        prev_pts_v = ov.pts_90k;
        prev_pts_a = oa.pts_90k;

        CHECK(abs64(ov.utc_ms - wall_v) <= DRIFT_TOL_MS);
        CHECK(abs64(oa.utc_ms - wall_a) <= DRIFT_TOL_MS);

        raw_v += 66;
        raw_a += 64;
        wall_v += 66;
        wall_a += 64;
    }
    /* 只有首帧锚定算一次 discontinuity */
    CHECK_EQ_U32(zs_clock_discontinuity_count(cv), 1u);
    CHECK_EQ_U32(zs_clock_discontinuity_count(ca), 1u);
    zs_clock_destroy(cv);
    zs_clock_destroy(ca);
}

/**
 * 源端 PTS 以 3 倍速度推进（抖动累加、时间基错误的典型表现）。
 * 旧实现会把这段录像记成 3 倍时长；现在必须被墙钟限速压住。
 */
static void test_forward_drift_clamped(void)
{
    zs_clock *c = zs_clock_create(NULL);
    int64_t wall = WALL_BASE;
    int64_t raw = 0;
    int64_t last_utc = 0;
    int i;
    zs_clock_out o;

    CHECK(c != NULL);
    for (i = 0; i < 900; ++i) {
        zs_clock_input(c, raw * 90, wall, 0, &o);
        CHECK(o.valid);
        CHECK(o.utc_ms >= last_utc);
        CHECK(o.utc_ms <= wall + RESYNC_MS + 1);
        last_utc = o.utc_ms;
        raw += 200; /* 3x 于真实 66ms */
        wall += 66;
    }
    /* 限速是"跟着墙钟走"，不是整段冻死：最终仍应贴近墙钟而不是停在开头 */
    CHECK(abs64(last_utc - wall) <= DRIFT_TOL_MS);
    CHECK(last_utc - WALL_BASE > 55000);
    zs_clock_destroy(c);
}

/**
 * 相机重启导致 PTS 归零：必须重锚到墙钟，并标记 discontinuity。
 */
static void test_restart_reanchors(void)
{
    zs_clock *c = zs_clock_create(NULL);
    int64_t wall = WALL_BASE;
    int64_t raw = 0;
    int i;
    zs_clock_out o;

    CHECK(c != NULL);
    for (i = 0; i < 100; ++i) {
        zs_clock_input(c, raw * 90, wall, 0, &o);
        raw += 66;
        wall += 66;
    }
    CHECK_EQ_U32(zs_clock_discontinuity_count(c), 1u);

    /* 重启归零：时间戳回退，且墙钟已经往前走了 5 秒 */
    wall += 5000;
    zs_clock_input(c, 0, wall, 0, &o);
    CHECK(o.valid);
    CHECK_EQ_INT(o.discontinuity, 1);
    CHECK_EQ_U32(zs_clock_discontinuity_count(c), 2u);
    CHECK(abs64(o.utc_ms - wall) <= 1000);

    /* 重锚后继续按增量走 */
    zs_clock_input(c, 66 * 90, wall + 66, 0, &o);
    CHECK(o.valid);
    CHECK(abs64(o.utc_ms - (wall + 66)) <= 1000);
    zs_clock_destroy(c);
}

int main(void)
{
    RUN_TEST_COUNTED(test_independent_track_clocks);
    RUN_TEST_COUNTED(test_forward_drift_clamped);
    RUN_TEST_COUNTED(test_restart_reanchors);
    ZCS_TEST_SUMMARY("test_zs_clock");
}
