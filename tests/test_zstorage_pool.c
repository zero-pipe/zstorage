/**
 * @file test_zstorage_pool.c
 * @brief zstorage 二期：槽位池（容量/回收/钉住/范围检索）与单通道写入器。
 */
#include "storage/zs_config.h"
#include "storage/zs_pool.h"
#include "storage/zs_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <process.h>
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

#define TEST_ROOT "./test_zstorage_pool"
#define SLOT_BYTES (512ULL * 1024ULL)
#define INDEX_BYTES (8u * 1024u)

/* 每次运行用独立子目录，避免上次遗留的槽位影响断言。 */
static char g_root[128];

static const char *run_dir(const char *name)
{
    static char buf[256];

    snprintf(buf, sizeof(buf), "%s/%s", g_root, name);
    return buf;
}

static size_t make_ps_pack(uint8_t *b, size_t payload_len, unsigned char fill)
{
    size_t off = 14;
    size_t len = payload_len + 3;

    b[0] = 0x00;
    b[1] = 0x00;
    b[2] = 0x01;
    b[3] = 0xBA;
    memset(b + 4, 0x44, 9);
    b[13] = 0xF8;
    b[off + 0] = 0x00;
    b[off + 1] = 0x00;
    b[off + 2] = 0x01;
    b[off + 3] = 0xE0;
    b[off + 4] = (uint8_t)(len >> 8);
    b[off + 5] = (uint8_t)(len & 0xFF);
    b[off + 6] = 0x80;
    b[off + 7] = 0x00;
    b[off + 8] = 0x00;
    memset(b + off + 9, fill, payload_len);
    return off + 6 + len;
}

static zs_pool_opts pool_opts(const char *dir, uint64_t capacity, uint32_t retention_days)
{
    zs_pool_opts o;

    memset(&o, 0, sizeof(o));
    o.dir = dir;
    o.capacity_bytes = capacity;
    o.retention_days = retention_days;
    o.slot_bytes = SLOT_BYTES;
    o.index_bytes = INDEX_BYTES;
    o.free_watermark_pct = 5;
    return o;
}

/* 申请一个槽位，写一帧并封口归还。 */
static int write_one_slot(zs_pool *p, uint32_t channel, uint64_t utc_ms, uint32_t *out_id)
{
    zs_pool_lease lease;
    uint8_t pack[2048];
    size_t n;

    if (zs_pool_acquire(p, channel, "cam", utc_ms, &lease) != 0) {
        return -1;
    }
    n = make_ps_pack(pack, 600, 'X');
    if (zs_slot_append(lease.slot, pack, n) != 0 ||
        zs_slot_add_keyframe(lease.slot, utc_ms, (int64_t)utc_ms * 90, zs_slot_write_offset(lease.slot),
                             0) != 0) {
        (void)zs_pool_release(p, &lease);
        return -1;
    }
    if (out_id) {
        *out_id = lease.id;
    }
    return zs_pool_release(p, &lease);
}

static int test_pool_defaults(void)
{
    zs_pool_opts o;

    zs_pool_opts_default(&o);
    CHECK(o.capacity_bytes == 2ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
    CHECK(o.retention_days == 7);
    CHECK(o.slot_bytes == 256ULL * 1024ULL * 1024ULL);
    CHECK(o.index_bytes == ZS_SLOT_DEFAULT_INDEX_BYTES);
    CHECK(o.free_watermark_pct == 5);
    return 0;
}

static int test_pool_evict_and_pin(void)
{
    zs_pool *p;
    zs_pool_opts o = pool_opts(run_dir("evict"), SLOT_BYTES * 2ULL, 7);
    zs_pool_lease l;
    uint32_t id1 = 0;
    uint32_t id2 = 0;
    uint32_t id3 = 0;

    p = zs_pool_open(&o);
    CHECK(p != NULL);
    CHECK(zs_pool_max_slots(p) == 2);

    CHECK(write_one_slot(p, 100, 1700000000000ULL, &id1) == 0);
    CHECK(write_one_slot(p, 100, 1700000010000ULL, &id2) == 0);
    CHECK(zs_pool_count(p) == 2);
    CHECK(zs_pool_free_count(p) == 0);

    /* 池满：申请应回收最旧的已封口槽位 */
    CHECK(write_one_slot(p, 100, 1700000020000ULL, &id3) == 0);
    CHECK(id3 == id1 || id3 == id2);
    CHECK(zs_pool_count(p) == 2);

    /* 两路都钉住后应申请失败 */
    {
        uint32_t i;
        uint32_t n = zs_pool_count(p);

        for (i = 0; i < n; ++i) {
            zs_pool_info info;

            CHECK(zs_pool_get_info(p, i, &info) == 0);
            (void)zs_pool_pin(p, info.id);
        }
        CHECK(zs_pool_acquire(p, 100, "cam", 1700000030000ULL, &l) != 0);

        for (i = 0; i < n; ++i) {
            zs_pool_info info;

            CHECK(zs_pool_get_info(p, i, &info) == 0);
            (void)zs_pool_unpin(p, info.id);
        }
    }
    CHECK(zs_pool_acquire(p, 100, "cam", 1700000030000ULL, &l) == 0);
    CHECK(zs_pool_release(p, &l) == 0);
    zs_pool_close(p);
    return 0;
}

static int test_pool_expire(void)
{
    zs_pool *p;
    zs_pool_opts o = pool_opts(run_dir("expire"), SLOT_BYTES * 4ULL, 1); /* 保留 1 天 */
    uint64_t day = 86400000ULL;
    uint64_t now = 1700000000000ULL;

    p = zs_pool_open(&o);
    CHECK(p != NULL);
    CHECK(write_one_slot(p, 1, now - 2 * day, NULL) == 0); /* 已过期 */
    CHECK(write_one_slot(p, 1, now - day / 2, NULL) == 0); /* 未过期 */
    CHECK(zs_pool_expire(p, now) == 1);
    CHECK(zs_pool_free_count(p) == 1);
    CHECK(zs_pool_expire(p, now) == 0);
    zs_pool_close(p);
    return 0;
}

static int test_pool_reopen(void)
{
    zs_pool *p;
    zs_pool_opts o = pool_opts(run_dir("reopen"), SLOT_BYTES * 4ULL, 7);
    uint32_t ids[4];
    uint32_t n;
    uint64_t now = 1700000000000ULL;

    p = zs_pool_open(&o);
    CHECK(p != NULL);
    CHECK(write_one_slot(p, 1, now, NULL) == 0);
    CHECK(write_one_slot(p, 1, now + 10000, NULL) == 0);
    zs_pool_close(p);

    p = zs_pool_open(&o);
    CHECK(p != NULL);
    CHECK(zs_pool_count(p) == 2);
    n = zs_pool_find_range(p, now, now + 20000, ids, 4);
    CHECK(n == 2);
    {
        zs_pool_info a;
        zs_pool_info b;

        CHECK(zs_pool_get_info_by_id(p, ids[0], &a) == 0);
        CHECK(zs_pool_get_info_by_id(p, ids[1], &b) == 0);
        CHECK(a.seg_start_ms <= b.seg_start_ms);
        CHECK(a.state == ZS_SLOT_SEALED);
    }
    zs_pool_close(p);
    return 0;
}

static int test_writer_rotate(void)
{
    zs_pool *p;
    zs_pool_opts po = pool_opts(run_dir("writer"), SLOT_BYTES * 8ULL, 7);
    zs_writer_opts wo;
    zs_writer *w;
    uint8_t pack[2048];
    size_t pl;
    uint64_t now = 1700000000000ULL;
    int i;
    int j;
    uint32_t ids[16];
    uint32_t n;

    p = zs_pool_open(&po);
    CHECK(p != NULL);
    memset(&wo, 0, sizeof(wo));
    wo.pool = p;
    wo.channel_id = 42;
    wo.stream = "live/1001_0001";
    wo.flush_bytes = 4096;
    wo.flush_ms = 1000;
    wo.slot_max_ms = 1000;
    wo.slot_seal_margin = 64 * 1024;
    w = zs_writer_create(&wo);
    CHECK(w != NULL);

    pl = make_ps_pack(pack, 900, 'Y');
    for (i = 0; i < 18; ++i) {
        int key = (i % 3) == 0;

        now += 500; /* 每帧 500ms -> slot_max_ms=1000 时约每 3 帧换槽 */
        zs_writer_begin_frame(w, key, (int64_t)i * 90000, (int64_t)now, now);
        for (j = 0; j < 3; ++j) {
            CHECK(zs_writer_push(w, pack, pl, now) == 0);
        }
        CHECK(zs_writer_tick(w, now) == 0);
    }
    CHECK(zs_writer_rotations(w) >= 2);
    CHECK(zs_writer_bytes(w) == (uint64_t)18 * 3 * pl);
    zs_writer_close(w);
    zs_writer_destroy(w);

    n = zs_pool_find_range(p, 1700000000000ULL, now + 1000, ids, 16);
    CHECK(n >= 2);
    for (i = 0; i < (int)n; ++i) {
        zs_slot *s = zs_pool_open_slot(p, ids[i]);
        zs_keyframe_entry e;
        uint8_t head[4];
        uint32_t k;

        CHECK(s != NULL);
        CHECK(zs_slot_keyframe_count(s) > 0);
        /* 每个槽位第一个关键帧必须落在 PS 包头上 */
        CHECK(zs_slot_get_keyframe(s, 0, &e) == 0);
        CHECK(e.file_offset == zs_slot_data_start(s));
        CHECK(zs_slot_pread(s, head, 4, e.file_offset) == 0);
        CHECK(head[0] == 0 && head[1] == 0 && head[2] == 1 && head[3] == 0xBA);
        /* 索引时间必须单调 */
        for (k = 1; k < zs_slot_keyframe_count(s); ++k) {
            zs_keyframe_entry prev;

            CHECK(zs_slot_get_keyframe(s, k - 1, &prev) == 0);
            CHECK(zs_slot_get_keyframe(s, k, &e) == 0);
            CHECK(e.utc_ms >= prev.utc_ms);
            CHECK(e.file_offset > prev.file_offset);
        }
        zs_slot_close(s);
    }
    zs_pool_close(p);
    return 0;
}

static int test_writer_waits_keyframe(void)
{
    zs_pool *p;
    zs_pool_opts po = pool_opts(run_dir("waitkey"), SLOT_BYTES * 4ULL, 7);
    zs_writer_opts wo;
    zs_writer *w;
    uint8_t pack[2048];
    size_t pl;

    p = zs_pool_open(&po);
    CHECK(p != NULL);
    memset(&wo, 0, sizeof(wo));
    wo.pool = p;
    wo.channel_id = 7;
    wo.stream = "live/1001_0002";
    w = zs_writer_create(&wo);
    CHECK(w != NULL);
    pl = make_ps_pack(pack, 300, 'Z');

    zs_writer_begin_frame(w, 0, 0, 1700000000000LL, 1700000000000ULL);
    CHECK(zs_writer_push(w, pack, pl, 1700000000000ULL) == 0); /* 丢弃，不报错 */
    CHECK(!zs_writer_active(w));
    CHECK(zs_writer_bytes(w) == 0);

    zs_writer_begin_frame(w, 1, 0, 1700000000500LL, 1700000000500ULL);
    CHECK(zs_writer_active(w));
    CHECK(zs_writer_push(w, pack, pl, 1700000000500ULL) == 0);
    CHECK(zs_writer_bytes(w) == pl);
    zs_writer_close(w);
    zs_writer_destroy(w);
    zs_pool_close(p);
    return 0;
}

static int test_config_bridge(void)
{
    zcs_config cfg;
    zs_pool_opts po;
    zs_writer_opts wo;

    zcs_config_default(&cfg);
    /* 默认 2TB / 7 天 */
    CHECK(cfg.storage.capacity_gb == 2048);
    CHECK(cfg.storage.retention_days == 7);

    zs_pool_opts_from_config(&po, &cfg.storage);
    CHECK(po.capacity_bytes == 2048ULL * ZS_GB);
    CHECK(po.retention_days == 7);
    CHECK(po.slot_bytes == 256ULL * ZS_MB);
    CHECK(po.index_bytes == 1024u * 1024u);

    zs_writer_opts_from_config(&wo, &cfg.storage, NULL, 9, "cam1");
    CHECK(wo.channel_id == 9);
    CHECK(wo.flush_bytes == 512u * 1024u);
    CHECK(wo.flush_ms == 1000u);
    CHECK(wo.slot_max_ms == 900u * 1000u);
    CHECK(wo.slot_seal_margin == 4ULL * ZS_MB);

    /* 0/负值全部退回默认，不允许把池算成 0 容量 */
    cfg.storage.capacity_gb = 0;
    cfg.storage.slot_size_mb = -1;
    zs_pool_opts_from_config(&po, &cfg.storage);
    CHECK(po.capacity_bytes == 2ULL * 1024ULL * ZS_GB);
    CHECK(po.slot_bytes == 256ULL * ZS_MB);
    return 0;
}

int main(void)
{
#if defined(_WIN32)
    _mkdir(TEST_ROOT);
    snprintf(g_root, sizeof(g_root), TEST_ROOT "/run_%d", (int)_getpid());
    _mkdir(g_root);
#else
    mkdir(TEST_ROOT, 0755);
    snprintf(g_root, sizeof(g_root), TEST_ROOT "/run_%d", (int)getpid());
    mkdir(g_root, 0755);
#endif

    if (test_pool_defaults() != 0) {
        return 1;
    }
    if (test_config_bridge() != 0) {
        return 1;
    }
    if (test_pool_evict_and_pin() != 0) {
        return 1;
    }
    if (test_pool_expire() != 0) {
        return 1;
    }
    if (test_pool_reopen() != 0) {
        return 1;
    }
    if (test_writer_rotate() != 0) {
        return 1;
    }
    if (test_writer_waits_keyframe() != 0) {
        return 1;
    }
    printf("test_zstorage_pool: OK\n");
    return 0;
}
