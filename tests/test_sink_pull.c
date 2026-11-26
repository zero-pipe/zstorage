/**
 * @file test_sink_pull.c
 * @brief 播放输出统一取数契约（zms/play/sink.h）：缓冲不足时扩容重试、数据不丢。
 */
#include "zms/play/sink.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define BIG_CHUNK (600u * 1024u)

/* 假 muxer：一块固定大小的数据；缓冲不够时返回 NEED_BIGGER，够时吐出数据（只吐一次）。 */
typedef struct fake_muxer {
    size_t need;
    int served;
    unsigned char pattern;
    int force_zero;
    int force_error;
} fake_muxer;

static int fake_next(void *muxer, uint8_t *out, size_t cap, size_t *out_len)
{
    fake_muxer *f = (fake_muxer *)muxer;

    if (f->force_error) {
        return -1;
    }
    if (f->force_zero || f->served) {
        return 0;
    }
    if (!out || cap < f->need) {
        if (out_len) {
            *out_len = f->need;
        }
        return ZMS_SINK_NEED_BIGGER;
    }
    memset(out, f->pattern, f->need);
    f->served = 1;
    if (out_len) {
        *out_len = f->need;
    }
    return 1;
}

static int test_grow_and_serve(void)
{
    fake_muxer f = { BIG_CHUNK, 0, 0x5a, 0, 0 };
    zn_buf *buf = NULL;
    size_t len = 0;
    int r = zms_sink_pull(fake_next, &f, NULL, 128u * 1024u, 4u * 1024u * 1024u, &buf, &len);

    CHECK(r == 1);
    CHECK(buf != NULL);
    CHECK(len == BIG_CHUNK);
    CHECK(zn_buf_len(buf) == BIG_CHUNK);
    {
        const unsigned char *p = (const unsigned char *)zn_buf_data(buf);
        CHECK(p[0] == 0x5a && p[BIG_CHUNK - 1] == 0x5a);
    }
    zn_buf_unref(buf);
    /* 数据只给一次：再取应为“暂无数据” */
    buf = NULL;
    len = 0;
    CHECK(zms_sink_pull(fake_next, &f, NULL, 4096, 4u * 1024u * 1024u, &buf, &len) == 0);
    CHECK(buf == NULL);
    return 0;
}

static int test_over_limit(void)
{
    fake_muxer f = { BIG_CHUNK, 0, 1, 0, 0 };
    zn_buf *buf = NULL;
    size_t len = 0;
    int r = zms_sink_pull(fake_next, &f, NULL, 4096, 64u * 1024u, &buf, &len);

    CHECK(r == ZN_ERR_BUFFER_TOO_SMALL);
    CHECK(buf == NULL);
    return 0;
}

static int test_no_data_and_error(void)
{
    fake_muxer f = { BIG_CHUNK, 0, 1, 1, 0 };
    zn_buf *buf = NULL;
    size_t len = 0;

    CHECK(zms_sink_pull(fake_next, &f, NULL, 4096, 4096, &buf, &len) == 0);
    CHECK(buf == NULL);
    f.force_zero = 0;
    f.force_error = 1;
    CHECK(zms_sink_pull(fake_next, &f, NULL, 4096, 4096, &buf, &len) < 0);
    CHECK(buf == NULL);
    return 0;
}

int main(void)
{
    int r;

    if ((r = test_grow_and_serve()) != 0) {
        return r;
    }
    if ((r = test_over_limit()) != 0) {
        return r;
    }
    if ((r = test_no_data_and_error()) != 0) {
        return r;
    }
    printf("test_sink_pull: ok\n");
    return 0;
}
