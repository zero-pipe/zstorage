/**
 * @file test_egress_clock.c
 * @brief 出站播放时钟：epoch 锁定、相对时间、RTP 时间戳、pacing 到期判断、暂停/缩放。
 *
 * 为什么测它：播放侧所有时间换算（RTP ts、RTCP SR NTP、pacing 是否该发帧）
 * 都基于这个结构。此前它没有任何测试，而 egress/pacing 是并发路径，
 * 一旦算错，表现是"偶发卡顿 / 音画不同步"，极难从日志定位。
 *
 * 时钟实现依赖墙钟（zcs_wall_ms），故所有"是否到期"的判断都写宽容断言：
 * 验证的是方向与边界，不是精确毫秒。
 */
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/media/clock.h"
#include "zb/os/platform.h"
#include "zcs_test.h"

static void test_init_and_lock(void)
{
    zcs_egress_clock c;

    zcs_egress_clock_init(&c);
    CHECK(!zcs_egress_clock_epoch_locked(&c));
    CHECK(c.play_scale == 1.0);
    /* 未锁定时 rel_ms 恒为 0，rtp_ts 也为 0 */
    CHECK(zcs_egress_clock_rel_ms(&c, 1234u) == 0u);
    CHECK(zcs_egress_clock_rtp_ts(&c, 1234u, 90000u) == 0u);

    /* 首次锁定返回 1，重复锁定返回 0 且不改变 epoch */
    CHECK(zcs_egress_clock_lock_epoch(&c, 5000u) == 1);
    CHECK(zcs_egress_clock_epoch_locked(&c));
    CHECK_EQ_U32(c.epoch_ms, 5000u);
    CHECK_EQ_INT(zcs_egress_clock_lock_epoch(&c, 6000u), 0);
    CHECK_EQ_U32(c.epoch_ms, 5000u);

    /* rel_ms：早于 epoch 钳到 0，等于 epoch 为 0，之后为差值 */
    CHECK_EQ_U32(zcs_egress_clock_rel_ms(&c, 4000u), 0u);
    CHECK_EQ_U32(zcs_egress_clock_rel_ms(&c, 5000u), 0u);
    CHECK_EQ_U32(zcs_egress_clock_rel_ms(&c, 6000u), 1000u);

    /* unlock 回到未锁状态 */
    zcs_egress_clock_unlock(&c);
    CHECK(!zcs_egress_clock_epoch_locked(&c));
}

static void test_rtp_ts_conversion(void)
{
    zcs_egress_clock c;
    uint32_t ts;

    zcs_egress_clock_init(&c);
    CHECK(zcs_egress_clock_lock_epoch(&c, 1000u) == 1);

    /* 视频 90kHz：1ms = 90 ticks */
    ts = zcs_egress_clock_rtp_ts(&c, 1000u + 1000u, 90000u);
    CHECK_EQ_U32(ts, 90000u); /* 90kHz: 1000ms = 90000 ticks */

    /* 音频 8kHz：1ms = 8 ticks */
    ts = zcs_egress_clock_rtp_ts(&c, 1000u + 1000u, 8000u);
    CHECK_EQ_U32(ts, 8000u);

    /* 音频 48kHz */
    ts = zcs_egress_clock_rtp_ts(&c, 1000u + 500u, 48000u);
    CHECK_EQ_U32(ts, 24000u);

    /* clock_hz=0 回退 90kHz，不应除零 */
    ts = zcs_egress_clock_rtp_ts_rel(1000u, 0u);
    CHECK_EQ_U32(ts, 90000u);

    /* 时间戳随 dts 单调递增 */
    CHECK(zcs_egress_clock_rtp_ts(&c, 2001u, 90000u) >
          zcs_egress_clock_rtp_ts(&c, 2000u, 90000u));
}

static void test_media_due(void)
{
    zcs_egress_clock c;

    zcs_egress_clock_init(&c);

    /* 未锁定：一律视为"可以发"，让首帧不被 pacing 卡住 */
    CHECK(zcs_egress_clock_media_due(&c, 100u, 350u) != 0);

    zcs_egress_clock_lock_epoch(&c, 1000u);

    /* lead_ms 内：rel=0 立即到期 */
    CHECK(zcs_egress_clock_media_due(&c, 1000u, 350u) != 0);

    /* 远未来（rel 远超已过墙钟 + lead）：不应到期 */
    CHECK(zcs_egress_clock_media_due(&c, 600000u, 350u) == 0);

    /* 暂停期间一律不到期 */
    zcs_egress_clock_pause(&c);
    CHECK(zcs_egress_clock_is_paused(&c));
    CHECK(zcs_egress_clock_media_due(&c, 1000u, 350u) == 0);
    zcs_egress_clock_resume(&c);
    CHECK(!zcs_egress_clock_is_paused(&c));
    CHECK(zcs_egress_clock_media_due(&c, 1000u, 350u) != 0);
}

static void test_pause_resume_keeps_timeline(void)
{
    zcs_egress_clock c;
    uint64_t wall_before;
    uint64_t wall_after;

    zcs_egress_clock_init(&c);
    zcs_egress_clock_lock_epoch(&c, 0u);

    /* 暂停 50ms 后恢复：play_wall 应前移暂停时长，避免"暂停即追帧" */
    wall_before = c.play_wall_ms;
    zcs_egress_clock_pause(&c);
    zb_sleep_ms(50);
    zcs_egress_clock_resume(&c);
    wall_after = c.play_wall_ms;

    CHECK(wall_after >= wall_before + 40u); /* 实测 50ms，留调度误差 */
    CHECK(wall_after <= wall_before + 500u);

    /* 未暂停时重复 resume 不应改变时间基准 */
    {
        uint64_t w = c.play_wall_ms;
        zcs_egress_clock_resume(&c);
        CHECK(c.play_wall_ms == w);
    }
}

static void test_scale(void)
{
    zcs_egress_clock c;

    zcs_egress_clock_init(&c);

    /* 非法 scale 回退 1.0 */
    zcs_egress_clock_set_scale(&c, 0.0);
    CHECK(c.play_scale == 1.0);
    zcs_egress_clock_set_scale(&c, -2.0);
    CHECK(c.play_scale == 1.0);
    zcs_egress_clock_set_scale(&c, 99.0);
    CHECK(c.play_scale == 1.0);

    /* 合法范围生效（RTSP Scale 头：0.25x ~ 16x） */
    zcs_egress_clock_set_scale(&c, 2.0);
    CHECK(c.play_scale == 2.0);
    zcs_egress_clock_set_scale(&c, 16.0);
    CHECK(c.play_scale == 16.0);
    zcs_egress_clock_set_scale(&c, 0.25);
    CHECK(c.play_scale == 0.25);
}

static void test_rebase_and_sync_wall(void)
{
    zcs_egress_clock c;

    zcs_egress_clock_init(&c);
    zcs_egress_clock_lock_epoch(&c, 1000u);

    /* rebase：epoch 换成新锚点，且保证 rel 从 0 重新计 */
    zcs_egress_clock_rebase(&c, 9000u);
    CHECK(c.epoch_locked);
    CHECK(c.epoch_ms == 9000u);
    CHECK(zcs_egress_clock_rel_ms(&c, 9000u) == 0u);
    CHECK(zcs_egress_clock_rel_ms(&c, 9500u) == 500u);

    /* sync_wall：epoch 不变（RTP 保持单调），仅把 play_wall 对齐到当前 dts */
    zcs_egress_clock_lock_epoch(&c, 0u); /* 已锁定，无效调用 */
    zcs_egress_clock_sync_wall(&c, 9000u);
    CHECK(c.epoch_ms == 9000u);
}

static void test_sr_ntp_monotonic(void)
{
    zcs_egress_clock c;
    uint32_t sec1, frac1, sec2, frac2;

    zcs_egress_clock_init(&c);
    zcs_egress_clock_arm(&c);
    zcs_egress_clock_lock_epoch(&c, 0u);

    zcs_egress_clock_sr_ntp(&c, 0u, 90000u, &sec1, &frac1);
    zcs_egress_clock_sr_ntp(&c, 90000u, 90000u, &sec2, &frac2);

    /* rtp_ts 前进 1 秒 → NTP 也至少前进 1 秒（或秒内分数递增） */
    CHECK(sec2 > sec1 || (sec2 == sec1 && frac2 > frac1));

    /* 未锁定的时钟也要给出合法 NTP（不能返回 0/0） */
    {
        zcs_egress_clock fresh;
        uint32_t s, f;

        zcs_egress_clock_init(&fresh);
        zcs_egress_clock_sr_ntp(&fresh, 0u, 90000u, &s, &f);
        CHECK(s != 0u);
    }
}

int main(void)
{
    RUN_TEST_COUNTED(test_init_and_lock);
    RUN_TEST_COUNTED(test_rtp_ts_conversion);
    RUN_TEST_COUNTED(test_media_due);
    RUN_TEST_COUNTED(test_pause_resume_keeps_timeline);
    RUN_TEST_COUNTED(test_scale);
    RUN_TEST_COUNTED(test_rebase_and_sync_wall);
    RUN_TEST_COUNTED(test_sr_ntp_monotonic);
    ZCS_TEST_SUMMARY("test_egress_clock");
}
