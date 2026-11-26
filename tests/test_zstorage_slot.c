/**
 * @file test_zstorage_slot.c
 * @brief zstorage 一期：时间戳归一化 + 预分配槽位（创建/追加/索引/恢复）。
 *
 * 不依赖网络与真实码流：PS pack 由本文件按规范合成为可被 zs_ps_pack_len
 * 识别的字节序列，用来验证"从任意关键帧偏移切入都能定位完整 pack"。
 */
#include "storage/zs_clock.h"
#include "storage/zs_slot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define TEST_DIR "./test_zstorage_slot"

static const char *slot_path(const char *name)
{
    static char buf[512];

    snprintf(buf, sizeof(buf), "%s/%s", TEST_DIR, name);
    return buf;
}

/* 合成一个标准 PS pack：pack header + 一个 PES。返回总字节数。 */
static size_t make_ps_pack(uint8_t *b, size_t payload_len, unsigned char fill)
{
    size_t off = 14;
    size_t len = payload_len + 3;

    b[0] = 0x00;
    b[1] = 0x00;
    b[2] = 0x01;
    b[3] = 0xBA;
    memset(b + 4, 0x44, 9); /* SCR / program_mux_rate 占位 */
    b[13] = 0xF8;           /* stuffing 长度 0 */
    b[off + 0] = 0x00;
    b[off + 1] = 0x00;
    b[off + 2] = 0x01;
    b[off + 3] = 0xE0; /* video stream */
    b[off + 4] = (uint8_t)(len >> 8);
    b[off + 5] = (uint8_t)(len & 0xFF);
    b[off + 6] = 0x80; /* PES flags 占位 */
    b[off + 7] = 0x00;
    b[off + 8] = 0x00;
    memset(b + off + 9, fill, payload_len);
    return off + 6 + len;
}

static int test_clock(void)
{
    zs_clock *c = zs_clock_create(NULL);
    zs_clock_out o;
    int64_t wall = 1700000000000LL;
    int64_t pts = 90000;
    int i;
    int64_t last = -1;

    CHECK(c != NULL);
    for (i = 0; i < 250; ++i) { /* 10 秒，25fps */
        zs_clock_input(c, pts, wall + (int64_t)i * 40, 40, &o);
        CHECK(o.valid);
        CHECK(o.pts_90k > last);
        last = o.pts_90k;
        if (i == 0) {
            CHECK(o.discontinuity == 1);
            CHECK(o.utc_ms == wall);
        } else {
            CHECK(o.discontinuity == 0);
        }
        pts += 3600;
    }
    CHECK(o.utc_ms == wall + 249 * 40);
    zs_clock_destroy(c);
    return 0;
}

static int test_clock_wrap(void)
{
    zs_clock *c = zs_clock_create(NULL);
    zs_clock_out o;
    int64_t wrap = (int64_t)1 << 33;
    int64_t wall = 1700000000000LL;
    int64_t pts = wrap - 3600 * 5;
    int i;
    int64_t last = -1;

    CHECK(c != NULL);
    for (i = 0; i < 20; ++i) { /* 跨越 2^33 回绕 */
        zs_clock_input(c, pts, wall + (int64_t)i * 40, 40, &o);
        CHECK(o.valid);
        CHECK(o.pts_90k > last);
        if (i > 0) {
            CHECK(o.discontinuity == 0); /* 回绕不是断流 */
            CHECK(o.pts_90k - last == 3600);
        }
        last = o.pts_90k;
        pts = (pts + 3600) & (((int64_t)1 << 33) - 1);
    }
    CHECK(zs_clock_discontinuity_count(c) == 1);
    zs_clock_destroy(c);
    return 0;
}

static int test_clock_restart(void)
{
    zs_clock *c = zs_clock_create(NULL);
    zs_clock_out o;
    int64_t wall = 1700000000000LL;
    int64_t last;

    CHECK(c != NULL);
    zs_clock_input(c, 100000, wall, 40, &o);
    zs_clock_input(c, 103600, wall + 40, 40, &o);
    last = o.pts_90k;
    /* 相机重启：PTS 归零，墙钟已过 3 秒 */
    zs_clock_input(c, 0, wall + 3000, 40, &o);
    CHECK(o.discontinuity == 1);
    CHECK(o.pts_90k > last);
    CHECK(o.utc_ms >= wall + 3000);
    CHECK(zs_clock_discontinuity_count(c) == 2); /* 首帧 + 重启 */
    zs_clock_destroy(c);
    return 0;
}

static int test_slot_basic(void)
{
    zs_slot_opts opts;
    zs_slot *s;
    zs_slot *r;
    zs_keyframe_entry e;
    uint8_t buf[8192];
    uint8_t head[4];
    size_t n;
    uint64_t off0;
    uint64_t off1;
    uint64_t off2;
    int i;

    memset(&opts, 0, sizeof(opts));
    opts.channel_id = 1001;
    opts.slot_seq = 7;
    opts.slot_bytes = 1024 * 1024;
    opts.index_bytes = 4096; /* 128 条关键帧 */
    s = zs_slot_create(slot_path("basic.dat"), &opts);
    CHECK(s != NULL);
    CHECK(zs_slot_state(s) == ZS_SLOT_WRITING);
    CHECK(zs_slot_channel_id(s) == 1001);
    CHECK(zs_slot_data_start(s) == 4096 + 4096);

    n = make_ps_pack(buf, 1000, 'A');
    for (i = 0; i < 3; ++i) {
        uint64_t at = zs_slot_write_offset(s);

        CHECK(zs_slot_append(s, buf, n) == 0);
        CHECK(zs_slot_add_keyframe(s, 1700000000000ULL + (uint64_t)i * 1000ULL,
                                   (int64_t)i * 90000, at, 0) == 0);
    }
    off0 = zs_slot_data_start(s);
    off1 = off0 + n;
    off2 = off1 + n;
    CHECK(zs_slot_keyframe_count(s) == 3);
    CHECK(zs_slot_segment_start_ms(s) == 1700000000000ULL);
    CHECK(zs_slot_segment_end_ms(s) == 1700000002000ULL);
    CHECK(zs_slot_seal(s) == 0);
    CHECK(zs_slot_state(s) == ZS_SLOT_SEALED);
    zs_slot_close(s);

    r = zs_slot_open(slot_path("basic.dat"));
    CHECK(r != NULL);
    CHECK(zs_slot_state(r) == ZS_SLOT_SEALED);
    CHECK(zs_slot_keyframe_count(r) == 3);
    CHECK(zs_slot_write_offset(r) == off2 + (uint64_t)n);

    CHECK(zs_slot_find(r, 1700000000000ULL, &e) == 0);
    CHECK(e.file_offset == off0);
    CHECK(zs_slot_find(r, 1700000001500ULL, &e) == 0);
    CHECK(e.file_offset == off1);
    CHECK(zs_slot_find(r, 1600000000000ULL, &e) == 1);
    CHECK(zs_slot_find(r, 1800000000000ULL, &e) == 0);
    CHECK(e.file_offset == off2);

    CHECK(zs_slot_pread(r, head, 4, off1) == 0);
    CHECK(head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xBA);
    zs_slot_close(r);
    return 0;
}

static int test_slot_recover(void)
{
    zs_slot_opts opts;
    zs_slot *s;
    zs_slot *r;
    uint8_t buf[8192];
    size_t n;
    uint64_t expect;
    uint64_t at;
    int i;

    memset(&opts, 0, sizeof(opts));
    opts.channel_id = 1002;
    opts.slot_seq = 1;
    opts.slot_bytes = 1024 * 1024;
    opts.index_bytes = 4096;
    s = zs_slot_create(slot_path("crash.dat"), &opts);
    CHECK(s != NULL);
    n = make_ps_pack(buf, 700, 'B');
    for (i = 0; i < 3; ++i) {
        at = zs_slot_write_offset(s);
        CHECK(zs_slot_append(s, buf, n) == 0);
        CHECK(zs_slot_add_keyframe(s, 1700000100000ULL + (uint64_t)i * 1000,
                                   (int64_t)i * 90000, at, 0) == 0);
    }
    expect = zs_slot_write_offset(s);
    /* 模拟断电：写半个 pack 头就停 */
    CHECK(zs_slot_append(s, buf, 10) == 0);
    CHECK(zs_slot_write_offset(s) == expect + 10);
    zs_slot_close(s); /* 未封口 */

    r = zs_slot_open(slot_path("crash.dat"));
    CHECK(r != NULL);
    CHECK(zs_slot_state(r) == ZS_SLOT_WRITING);
    CHECK(zs_slot_recover(r, 0) == 0);
    CHECK(zs_slot_write_offset(r) == expect); /* 末尾半帧被丢弃 */
    CHECK(zs_slot_keyframe_count(r) == 3);
    CHECK(zs_slot_state(r) == ZS_SLOT_WRITING);

    /* 恢复后继续追加应该正常 */
    at = zs_slot_write_offset(r);
    CHECK(zs_slot_append(r, buf, n) == 0);
    CHECK(zs_slot_add_keyframe(r, 1700000103000ULL, 3 * 90000, at, 0) == 0);
    CHECK(zs_slot_keyframe_count(r) == 4);
    CHECK(zs_slot_recover(r, 1) == 0);
    CHECK(zs_slot_state(r) == ZS_SLOT_SEALED);
    CHECK(zs_slot_keyframe_count(r) == 4);
    zs_slot_close(r);
    return 0;
}

static int test_slot_reject_bad_header(void)
{
    zs_slot_opts opts;
    zs_slot *s;
    FILE *fp;
    uint8_t junk = 0xFF;

    memset(&opts, 0, sizeof(opts));
    opts.slot_bytes = 512 * 1024;
    opts.index_bytes = 4096;
    s = zs_slot_create(slot_path("bad.dat"), &opts);
    CHECK(s != NULL);
    zs_slot_close(s);

    fp = fopen(slot_path("bad.dat"), "rb+");
    CHECK(fp != NULL);
    CHECK(fwrite(&junk, 1, 1, fp) == 1); /* 破坏魔数 */
    fclose(fp);
    CHECK(zs_slot_open(slot_path("bad.dat")) == NULL);
    return 0;
}

static int test_ps_pack_len(void)
{
    uint8_t buf[2048];
    size_t n = make_ps_pack(buf, 100, 'C');

    CHECK(zs_ps_pack_len(buf, n) == (long)n);
    CHECK(zs_ps_pack_len(buf, n - 1) == -1); /* 不完整返回不可判定 */
    CHECK(zs_ps_pack_len(buf, 13) == -1);
    buf[3] = 0xBB;
    CHECK(zs_ps_pack_len(buf, n) == -1);
    return 0;
}

int main(void)
{
#if defined(_WIN32)
    _mkdir(TEST_DIR);
#else
    mkdir(TEST_DIR, 0755);
#endif

    if (test_clock() != 0) {
        return 1;
    }
    if (test_clock_wrap() != 0) {
        return 1;
    }
    if (test_clock_restart() != 0) {
        return 1;
    }
    if (test_ps_pack_len() != 0) {
        return 1;
    }
    if (test_slot_basic() != 0) {
        return 1;
    }
    if (test_slot_recover() != 0) {
        return 1;
    }
    if (test_slot_reject_bad_header() != 0) {
        return 1;
    }
    printf("test_zstorage_slot: OK\n");
    return 0;
}
