/**
 * @file test_rtsp_splitter.c
 * @brief RTSP interleaved 分流回归：跨 chunk 分帧、文本与二进制混排、失步重同步。
 *
 * 为什么测它：拉流时 RTP 是「$」帧与 RTSP 文本共用一条 TCP 流，
 * 分帧器要在任意 chunk 边界上把残留字节拼回。线上曾出现录像中间
 * 断流 15 秒、帧率只有 1/3：根因是回拼路径用 clear+append，而
 * zn_byte_buf_clear() 会把 data[0] 写成 '\0'，把 interleaved 帧头的 '$'(0x24) 抹成 0，
 * 从此找不到帧界，直到 256KB 缓存溢出才恢复。
 *
 * 三个用例分别锁住：
 *   1) 异常 chunk 划分下每帧数据完整且顺序正确（首字节不被破坏）
 *   2) 开放流里夹杂 RTSP 文本响应时，文本照常上报、RTP 不丢
 *   3) 已经失步（长串垃圾）时能在下一个 '$' 重新对齐
 */
#include "zcs/protocol/rtsp/splitter.h"

#include "zcs_test.h"

#include <stdlib.h>

typedef struct frame_desc {
    uint8_t ch;
    uint16_t len;
    uint32_t seed;
} frame_desc;

typedef struct capture {
    const frame_desc *desc;
    int desc_n;
    int idx;
    int first_bad; /* -1 = 没有错 */
    int msgs;
    size_t bytes;
} capture;

static uint8_t pat(uint32_t seed, size_t j)
{
    return (uint8_t)(seed * 31u + (uint32_t)j * 17u + 13u);
}

static void on_rtp_cb(uint8_t ch, const uint8_t *data, size_t len, void *user)
{
    capture *c = (capture *)user;
    const frame_desc *d;
    size_t j;

    if (c->idx >= c->desc_n) {
        if (c->first_bad < 0) {
            c->first_bad = c->idx;
        }
        ++c->idx;
        return;
    }
    d = &c->desc[c->idx];
    if (ch != d->ch || len != (size_t)d->len) {
        if (c->first_bad < 0) {
            c->first_bad = c->idx;
        }
        ++c->idx;
        return;
    }
    for (j = 0; j < len; ++j) {
        if (data[j] != pat(d->seed, j)) {
            if (c->first_bad < 0) {
                c->first_bad = c->idx;
            }
            break;
        }
    }
    c->bytes += len;
    ++c->idx;
}

static void on_msg_cb(const zcs_rtsp_message *msg, void *user)
{
    capture *c = (capture *)user;

    (void)msg;
    ++c->msgs;
}

/** 把一组帧编成连续的 interleaved 字节流。 */
static size_t build_stream(uint8_t *out, size_t cap, const frame_desc *d, int n)
{
    size_t off = 0;
    int i;

    for (i = 0; i < n; ++i) {
        size_t j;

        if (off + 4 + d[i].len > cap) {
            return 0;
        }
        out[off++] = '$';
        out[off++] = d[i].ch;
        out[off++] = (uint8_t)(d[i].len >> 8);
        out[off++] = (uint8_t)(d[i].len & 0xff);
        for (j = 0; j < d[i].len; ++j) {
            out[off++] = pat(d[i].seed, j);
        }
    }
    return off;
}

static void fill_desc(frame_desc *d, int n)
{
    int i;

    for (i = 0; i < n; ++i) {
        d[i].ch = (uint8_t)((i % 2) ? 2 : 0);
        d[i].len = (uint16_t)(200 + (i * 137) % 1240);
        d[i].seed = (uint32_t)(i + 1);
    }
}

/** 用一组长度不规则的 chunk 喂入，模拟 TCP 分段。 */
static void feed_in_chunks(zcs_rtsp_splitter *s, const uint8_t *buf, size_t len)
{
    static const size_t sizes[] = {4096, 1, 7, 1460, 333, 5000, 2, 9000, 17, 12345, 64};
    size_t off = 0;
    size_t k = 0;

    while (off < len) {
        size_t n = sizes[k++ % (sizeof(sizes) / sizeof(sizes[0]))];

        if (n > len - off) {
            n = len - off;
        }
        zcs_rtsp_splitter_input(s, buf + off, n);
        off += n;
    }
}

static void test_interleaved_survives_chunk_splits(void)
{
    enum { N = 400, CAP = 800 * 1024 };
    frame_desc *desc = (frame_desc *)calloc(N, sizeof(*desc));
    uint8_t *buf = (uint8_t *)malloc(CAP);
    capture cap;
    zcs_rtsp_splitter_opts opts;
    zcs_rtsp_splitter *s;
    size_t len;

    CHECK(desc != NULL && buf != NULL);
    fill_desc(desc, N);
    len = build_stream(buf, CAP, desc, N);
    CHECK(len > 0);

    memset(&cap, 0, sizeof(cap));
    cap.desc = desc;
    cap.desc_n = N;
    cap.first_bad = -1;
    memset(&opts, 0, sizeof(opts));
    opts.on_message = on_msg_cb;
    opts.on_rtp = on_rtp_cb;
    opts.user = &cap;
    s = zcs_rtsp_splitter_create(&opts);
    CHECK(s != NULL);
    zcs_rtsp_splitter_enable_rtp(s, 1);

    feed_in_chunks(s, buf, len);

    CHECK_EQ_INT(cap.first_bad, -1);
    CHECK_EQ_INT(cap.idx, N);
    CHECK_EQ_SIZE(cap.bytes, len - (size_t)N * 4);
    CHECK_EQ_INT(cap.msgs, 0);

    zcs_rtsp_splitter_destroy(s);
    free(desc);
    free(buf);
}

static void test_text_message_between_frames(void)
{
    enum { N = 60, CAP = 256 * 1024 };
    static const char resp[] = "RTSP/1.0 200 OK\r\nCSeq: 7\r\n\r\n";
    frame_desc *desc = (frame_desc *)calloc(N, sizeof(*desc));
    uint8_t *buf = (uint8_t *)malloc(CAP);
    capture cap;
    zcs_rtsp_splitter_opts opts;
    zcs_rtsp_splitter *s;
    size_t off = 0;
    int i;
    int expect_msgs = 0;

    CHECK(desc != NULL && buf != NULL);
    fill_desc(desc, N);
    for (i = 0; i < N; ++i) {
        size_t n;
        size_t j;

        /* 每 5 帧插一个 RTSP 文本响应（开放流里保活/RTCP 处理响应就长这样） */
        if (i % 5 == 2) {
            n = sizeof(resp) - 1;
            memcpy(buf + off, resp, n);
            off += n;
            ++expect_msgs;
        }
        buf[off++] = '$';
        buf[off++] = desc[i].ch;
        buf[off++] = (uint8_t)(desc[i].len >> 8);
        buf[off++] = (uint8_t)(desc[i].len & 0xff);
        for (j = 0; j < desc[i].len; ++j) {
            buf[off++] = pat(desc[i].seed, j);
        }
    }

    memset(&cap, 0, sizeof(cap));
    cap.desc = desc;
    cap.desc_n = N;
    cap.first_bad = -1;
    memset(&opts, 0, sizeof(opts));
    opts.on_message = on_msg_cb;
    opts.on_rtp = on_rtp_cb;
    opts.user = &cap;
    s = zcs_rtsp_splitter_create(&opts);
    CHECK(s != NULL);
    zcs_rtsp_splitter_enable_rtp(s, 1);

    feed_in_chunks(s, buf, off);

    CHECK_EQ_INT(cap.first_bad, -1);
    CHECK_EQ_INT(cap.idx, N);
    CHECK_EQ_INT(cap.msgs, expect_msgs);

    zcs_rtsp_splitter_destroy(s);
    free(desc);
    free(buf);
}

static void test_resync_after_garbage(void)
{
    enum { N = 50, GARBAGE = 200 * 1024, CAP = 256 * 1024 };
    frame_desc *desc = (frame_desc *)calloc(N, sizeof(*desc));
    uint8_t *buf = (uint8_t *)calloc(1, GARBAGE + CAP);
    capture cap;
    zcs_rtsp_splitter_opts opts;
    zcs_rtsp_splitter *s;
    size_t len;
    size_t i;
    size_t off = 0;

    CHECK(desc != NULL && buf != NULL);
    fill_desc(desc, N);

    /* 垃圾里不含 '$' 也不含 CR/LF：仿真一段已经错位的二进制流 */
    for (i = 0; i < GARBAGE; ++i) {
        buf[i] = (uint8_t)(0x30 + (i * 7) % 60);
        if (buf[i] == '$' || buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = 0x41;
        }
    }
    off = GARBAGE;
    len = build_stream(buf + off, CAP, desc, N);
    CHECK(len > 0);
    off += len;

    memset(&cap, 0, sizeof(cap));
    cap.desc = desc;
    cap.desc_n = N;
    cap.first_bad = -1;
    memset(&opts, 0, sizeof(opts));
    opts.on_message = on_msg_cb;
    opts.on_rtp = on_rtp_cb;
    opts.user = &cap;
    s = zcs_rtsp_splitter_create(&opts);
    CHECK(s != NULL);
    zcs_rtsp_splitter_enable_rtp(s, 1);

    feed_in_chunks(s, buf, off);

    CHECK_EQ_INT(cap.first_bad, -1);
    CHECK_EQ_INT(cap.idx, N);

    zcs_rtsp_splitter_destroy(s);
    free(desc);
    free(buf);
}

int main(void)
{
    RUN_TEST_COUNTED(test_interleaved_survives_chunk_splits);
    RUN_TEST_COUNTED(test_text_message_between_frames);
    RUN_TEST_COUNTED(test_resync_after_garbage);
    ZCS_TEST_SUMMARY("test_rtsp_splitter");
}
