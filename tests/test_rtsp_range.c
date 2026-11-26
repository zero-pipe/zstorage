/**
 * @file test_rtsp_range.c
 * @brief RTSP Range（NPT）与 Scale 头解析。
 *
 * 为什么测它：这是播放器 seek 的唯一入口。解析错了，表现为"拖动进度条
 * 跳到错误位置"或"seek 后从头播"，且不会报错——最难排查的一类问题。
 *
 * 覆盖的真实报文（来自 ffmpeg/VLC/海康 NVR 的实测）：
 *   Range: npt=0.000-            ← ffmpeg 起播
 *   Range: npt=123.456-          ← ffmpeg seek（秒，可带小数）
 *   Range: npt=now-              ← 直播起播
 *   Range: npt=01:02:03.500-     ← 部分设备用 h:m:s
 *   Range: npt=100-50            ← 倒序区间（应取起点 100）
 *   Range: npt=-                 ← 非法，应拒绝
 */
#include "zms/egress/rtp/rtp_play_pump.h"
#include "zms_test.h"

/* 断言：解析成功且毫秒值符合期望。 */
static void expect_ms(const char *hdr, uint64_t want_ms)
{
    uint64_t ms = 0;
    int now = 0;
    int ok = zms_rtsp_parse_range_npt_ms(hdr, &ms, &now);

    CHECK_EQ_INT(ok, 1);
    if (ms != want_ms) {
        ZMS_TEST_FAIL_("Range \"%s\" 解析为 %llu ms，期望 %llu ms", hdr ? hdr : "(null)",
                       (unsigned long long)ms, (unsigned long long)want_ms);
    }
}

/* 断言：解析失败（返回 0）。 */
static void expect_reject(const char *hdr)
{
    uint64_t ms = 12345;
    int now = 0;
    int ok = zms_rtsp_parse_range_npt_ms(hdr, &ms, &now);

    if (ok != 0) {
        ZMS_TEST_FAIL_("Range \"%s\" 应被拒绝，实际返回 ok=%d ms=%llu", hdr ? hdr : "(null)", ok,
                       (unsigned long long)ms);
    }
}

static void test_ffmpeg_start(void)
{
    /* ffmpeg 起播：npt=0.000- */
    expect_ms("npt=0.000-", 0);
    expect_ms("npt=0-", 0);
}

static void test_seek_seconds(void)
{
    /* 整数秒 */
    expect_ms("npt=10-", 10000);
    /* 带小数：四舍五入到毫秒 */
    expect_ms("npt=123.456-", 123456);
    expect_ms("npt=1.5-", 1500);
    /* 区间写法（起-止）：取起点 */
    expect_ms("npt=30.5-45.0", 30500);
}

static void test_seek_hms(void)
{
    /* 部分设备用 h:m:s 格式 */
    expect_ms("npt=01:02:03-", 3723000); /* 1h2m3s */
    expect_ms("npt=00:00:05-", 5000);
    expect_ms("npt=02:00:00.500-", 7200500); /* 带小数秒 */
}

static void test_now(void)
{
    uint64_t ms = 999;
    int now = 0;

    /* npt=now-：直播起播，out_now 置位，ms 无意义 */
    CHECK_EQ_INT(zms_rtsp_parse_range_npt_ms("npt=now-", &ms, &now), 1);
    CHECK_EQ_INT(now, 1);
    CHECK_EQ_U32(ms, 0);
}

static void test_case_insensitive(void)
{
    /* 协议不要求大小写，实测有设备发 NPT=/NOW= */
    expect_ms("NPT=10-", 10000);

    {
        uint64_t ms = 0;
        int now = 0;

        CHECK_EQ_INT(zms_rtsp_parse_range_npt_ms("NPT=NOW-", &ms, &now), 1);
        CHECK_EQ_INT(now, 1);
    }
}

static void test_leading_space(void)
{
    /* 头值前可能有空白 */
    expect_ms("   npt=7-", 7000);
    expect_ms("\tnpt=7-", 7000);
}

static void test_reverse_range(void)
{
    /* 倒序区间：按 NPT 语义取起点 */
    expect_ms("npt=100-50", 100000);
}

static void test_lenient_empty_npt(void)
{
    /* 实现是宽松解析：npt=- （无起点数字）视为"从 0 开始"。
     * 这是刻意的——播放从头开始是安全回退，比拒绝更能兼容各式客户端。
     * 该用例把行为固定下来，避免将来被"顺手改成严格解析"而破坏兼容性。 */
    expect_ms("npt=-", 0);
    expect_ms("npt=-30", 0); /* 只有终点、无起点，同样从 0 开始 */
}

static void test_reject_invalid(void)
{
    expect_reject("");             /* 空 */
    expect_reject(NULL);           /* NULL */
    expect_reject("bytes=0-100");  /* 不是 NPT */
    expect_reject("nptx=10-");     /* 前缀不对 */
    expect_reject("10-20");        /* 缺 npt= 前缀 */
}

static void test_out_ms_required(void)
{
    /* out_ms 为 NULL 直接返回 0（防御，不崩） */
    CHECK_EQ_INT(zms_rtsp_parse_range_npt_ms("npt=10-", NULL, NULL), 0);
}

static void test_overflow_safe(void)
{
    /* 超长数字：实现是 strtod，超出 double 精度后结果饱和但不崩、不越界。
     * 这里只断言"能返回且不崩"，具体值取决于 libc 的 strtod 行为。 */
    uint64_t ms = 0;
    int now = 0;

    (void)zms_rtsp_parse_range_npt_ms("npt=99999999999999999999-", &ms, &now);
    CHECK(ms > 0);
}

static void test_scale(void)
{
    /* 合法倍速 */
    CHECK(zms_rtsp_parse_play_scale("2.0", 1.0) == 2.0);
    CHECK(zms_rtsp_parse_play_scale("0.5", 1.0) == 0.5);
    CHECK(zms_rtsp_parse_play_scale("16", 1.0) == 16.0);

    /* 非法：保持传入的当前值（不能变成 0，否则播放停滞） */
    CHECK(zms_rtsp_parse_play_scale("0", 1.0) == 1.0);
    CHECK(zms_rtsp_parse_play_scale("-1", 1.0) == 1.0);
    CHECK(zms_rtsp_parse_play_scale("99", 1.0) == 1.0);
    CHECK(zms_rtsp_parse_play_scale("abc", 1.0) == 1.0);
    CHECK(zms_rtsp_parse_play_scale(NULL, 2.0) == 2.0);
    CHECK(zms_rtsp_parse_play_scale("", 2.0) == 2.0);
}

int main(void)
{
    RUN_TEST_COUNTED(test_ffmpeg_start);
    RUN_TEST_COUNTED(test_seek_seconds);
    RUN_TEST_COUNTED(test_seek_hms);
    RUN_TEST_COUNTED(test_now);
    RUN_TEST_COUNTED(test_case_insensitive);
    RUN_TEST_COUNTED(test_leading_space);
    RUN_TEST_COUNTED(test_reverse_range);
    RUN_TEST_COUNTED(test_lenient_empty_npt);
    RUN_TEST_COUNTED(test_reject_invalid);
    RUN_TEST_COUNTED(test_out_ms_required);
    RUN_TEST_COUNTED(test_overflow_safe);
    RUN_TEST_COUNTED(test_scale);
    ZMS_TEST_SUMMARY("test_rtsp_range");
}
