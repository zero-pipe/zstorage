/**
 * @file queue.c
 * @brief 直播 GOP 缓存与多读者挂接/seek 实现。
 *
 * 并发模型（理解本文件的前提）：
 *   - **写者**（推流入站）在自身 poller 线程，持 r->mu 修改槽位；
 *   - **读者**（各协议播放）分散在各自的 poller 线程，**不持锁**读槽位。
 *
 * 不持锁不是偷懒，是因为读者线程不能阻塞在写者的锁上——一旦阻塞，
 * 该 poller 上其它所有会话都会被拖住。因此读者侧采用 seqlock
 * （序号 + 双端校验）保证读到的是自洽快照，见 reader_try_take_slot。
 *
 * 内存回收不能紧跟“槽位被覆盖”发生：读者可能刚读到旧的 buf 指针、还没
 * try_ref 成功。所以 buf 先进 retire 环延迟释放，等一个宽限期且确认没有
 * 读者处在临界区后才真正 unref，见 retire_buf / drain_retired。
 *
 * Copyright (c) zero-media-server
 */
#include "zcs/core/gop/queue.h"
#include "core/gop/queue_internal.h"
#include "zcs/media/media_limits.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zb/os/sync.h"
#include "zn/util/buf.h"
#include "zn/util/mpsc.h"
#include "zn/util/log.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER)
#include <windows.h>
#endif

static size_t oldest_idx(const zcs_gop_queue *r);
static size_t beginning_idx(const zcs_gop_queue *r);

int ring_slot_video_sync(const zcs_frame_slot *slot)
{
    if (!slot || !slot->pub.data || slot->pub.len == 0 || slot->pub.config_frame) {
        return 0;
    }
    if (slot->pub.track != ZCS_TRACK_VIDEO) {
        return 0;
    }
    if (slot->pub.codec == ZCS_CODEC_H265) {
        /* 接受 IDR 与 CRA（type 16-21）：libx265/FFmpeg 常发 CRA_NUT */
        return zcs_h265_annexb_is_sync_key(slot->pub.data, slot->pub.len);
    }
    if (slot->pub.keyframe) {
        return 1;
    }
    if (slot->pub.codec == ZCS_CODEC_H264) {
        return zcs_h264_annexb_is_sync_key(slot->pub.data, slot->pub.len);
    }
    return zcs_gop_slot_is_egress_sync(&slot->pub);
}

#define ZCS_GOP_QUEUE_WAKE_CAP 64u
#define ZCS_GOP_QUEUE_READER_WAKE_CAP 8u
/** 持锁快照唤醒目标；溢出时仍持锁 push。 */
#define ZCS_GOP_QUEUE_SIGNAL_SNAP 128

static unsigned g_default_target_gops = ZCS_GOP_QUEUE_TARGET_GOPS;
static unsigned g_default_cache_ms = ZCS_GOP_QUEUE_DEFAULT_CACHE_MS;
static size_t g_default_byte_budget = ZCS_GOP_QUEUE_DEFAULT_BYTE_BUDGET;

/*
 * 编译器/CPU 内存屏障。
 *
 * 必须有：seqlock 的“先递增序号再写数据，写完再递增”只有在屏障保护下才
 * 成立。缺了屏障，编译器可能重排读写顺序，读者就会看到“序号没变但数据
 * 已经改了”的假一致状态。平台手感各不相同，这里做最小封装。
 */
static void zcs_gop_barrier(void)
{
#if defined(_MSC_VER)
    MemoryBarrier();
#elif defined(__GNUC__) || defined(__clang__)
    __sync_synchronize();
#else
    /* 尽力而为 */
#endif
}

/*
 * 自旋等待时让出流水线的提示指令（x86 的 pause / ARM 的 yield）。
 *
 * 读者在 seqlock 重试循环里会自旋；裸自旋有两个问题：一是浪费执行单元，
 * 二是退出自旋时可能被流水线预测惩罚。pause 同时解决两者，并降低
 * 与写者争抢同一 cache line 的频率。
 */
static void ring_cpu_pause(void)
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#else
    /* 空操作 */
#endif
}

static unsigned clamp_target_gops(unsigned n)
{
    if (n < 1u) {
        return 1u;
    }
    if (n > ZCS_GOP_QUEUE_MAX_GOP) {
        return ZCS_GOP_QUEUE_MAX_GOP;
    }
    return n;
}

void zcs_gop_queue_set_default_target_gops(unsigned n)
{
    g_default_target_gops = clamp_target_gops(n);
}

void zcs_gop_queue_set_target_gops(zcs_gop_queue *r, unsigned n)
{
    if (!r) {
        return;
    }
    n = clamp_target_gops(n);
    zb_mutex_lock(r->mu);
    r->target_gops = n;
    zb_mutex_unlock(r->mu);
}

void zcs_gop_queue_set_default_cache_ms(unsigned ms)
{
    g_default_cache_ms = ms;
}

void zcs_gop_queue_set_cache_ms(zcs_gop_queue *r, unsigned ms)
{
    if (!r) {
        return;
    }
    zb_mutex_lock(r->mu);
    r->cache_ms = ms;
    zb_mutex_unlock(r->mu);
}

void zcs_gop_queue_set_default_byte_budget(size_t bytes)
{
    g_default_byte_budget = bytes;
}

void zcs_gop_queue_set_byte_budget(zcs_gop_queue *r, size_t bytes)
{
    if (!r) {
        return;
    }
    zb_mutex_lock(r->mu);
    r->byte_budget = (unsigned)(bytes > 0xffffffffu ? 0xffffffffu : bytes);
    zb_mutex_unlock(r->mu);
}

size_t zcs_gop_queue_bytes(const zcs_gop_queue *r)
{
    size_t n;

    if (!r) {
        return 0;
    }
    zb_mutex_lock(((zcs_gop_queue *)r)->mu);
    n = r->bytes;
    zb_mutex_unlock(((zcs_gop_queue *)r)->mu);
    return n;
}

static void reader_unref_held(zcs_gop_reader *rd)
{
    if (!rd || !rd->held_ref) {
        return;
    }
    zn_buf_unref(rd->held_ref);
    rd->held_ref = NULL;
}

void frame_mux_clear(zcs_gop_reader *rd)
{
    if (!rd) {
        return;
    }
    for (int i = 0; i < rd->mux_cnt; ++i) {
        if (rd->mux[i].buf) {
            zn_buf_unref(rd->mux[i].buf);
        }
        rd->mux[i].buf = NULL;
    }
    rd->mux_cnt = 0;
}

static int frame_mux_pick_best(const zcs_gop_reader *rd, uint32_t max_skew_ms)
{
    int idx_v = -1;
    int idx_a = -1;
    uint32_t min_v = UINT32_MAX;
    uint32_t min_a = UINT32_MAX;

    for (int i = 0; i < rd->mux_cnt; ++i) {
        if (rd->mux[i].pub.track == ZCS_TRACK_AUDIO) {
            if (rd->mux[i].pub.dts_ms < min_a) {
                min_a = rd->mux[i].pub.dts_ms;
                idx_a = i;
            }
        } else {
            if (rd->mux[i].pub.dts_ms < min_v) {
                min_v = rd->mux[i].pub.dts_ms;
                idx_v = i;
            }
        }
    }

    if (idx_v >= 0 && idx_a >= 0) {
        if (min_a + max_skew_ms < min_v) {
            return idx_a;
        }
        if (min_v + max_skew_ms < min_a) {
            return idx_v;
        }
        return (min_a <= min_v) ? idx_a : idx_v;
    }
    if (idx_a >= 0) {
        return idx_a;
    }
    if (idx_v >= 0) {
        return idx_v;
    }
    return -1;
}

static void free_slot(zcs_frame_slot *p)
{
    if (!p) {
        return;
    }
    p->seq++;
    zcs_gop_barrier();
    if (p->buf) {
        zn_buf_unref(p->buf);
    }
    p->buf = NULL;
    p->pub.data = NULL;
    p->pub.len = 0;
    zcs_gop_barrier();
    p->seq++;
}

/**
 * 延迟释放 buf（调用方持 mu）。
 * 无锁读者可能刚读到旧 slot->buf 指针，还没来得及 try_ref。直接 unref 会让它碰到已释放内存。
 * 这里把 buf 暂存 ZCS_GOP_QUEUE_RETIRE_GRACE 帧后再 unref：读者临界区是纳秒级，
 * 16 帧（60fps 下约 270ms）远大于它。队列满时只能立即释放最旧一项，这是保守的退化路径。
 */
/*
 * 把 buf 放进 retire 环，稍后才真正释放（写者持 mu 调用）。
 *
 * 为什么不能立即 unref：读者拿到 slot->buf 后要先做几次 seq 校验才 try_ref，
 * 这段窗口里 buf 必须还活着。retire 环是“延迟释放名单”，drain_retired 会
 * 在宽限期过去、且确认没有读者处于临界区之后才真正 unref。
 *
 * 环满时丢最旧的——那意味着宽限期已经被大幅超出，继续留着只会无限增长内存；
 * 丢最旧同时也是最安全的（时间最久、读者最不可能还引用它）。
 */
static void retire_buf(zcs_gop_queue *r, zn_buf *b)
{
    size_t tail;

    if (!r || !b) {
        return;
    }
    if (r->retire_n == ZCS_GOP_QUEUE_RETIRE_CAP) {
        zn_buf_unref(r->retire[r->retire_head].buf);
        r->retire_head = (r->retire_head + 1) % ZCS_GOP_QUEUE_RETIRE_CAP;
        r->retire_n--;
    }
    tail = (r->retire_head + r->retire_n) % ZCS_GOP_QUEUE_RETIRE_CAP;
    r->retire[tail].buf = b;
    r->retire[tail].at = r->write_idx;
    r->retire_n++;
}

/* 任一读者仍处于无锁临界区（读到旧 slot->buf 但尚未 try_ref）时返回 1。
 *
 * 这是 drain_retired 的第二个放行条件。只靠“宽限期”不够：读者所在线程
 * 若被调度挂起，宽限期到了它可能还卡在临界区里。所以再多一道显式检查。 */
static int ring_readers_in_cs(const zcs_gop_queue *r)
{
    for (int i = 0; i < r->reader_count; ++i) {
        const zcs_gop_reader *rd = r->readers[i];

        if (rd && rd->in_cs) {
            return 1;
        }
    }
    return 0;
}

/*
 * 尝试释放 retire 环里的 buf。两个放行条件必须同时满足：
 *   1. 该 buf 距离被 retire 已超过宽限期（at + GRACE <= write_idx）；
 *   2. 没有读者正处于无锁临界区。
 * 任一不满足就停在这里，留到下一次提交时再试——**宁可晚释放，不可早释放**，
 * 早释放就是 use-after-free。
 *
 * @param all 0=按时机释放；1=强制全放（仅销毁路径，已保证无读者）；
 *            2=无视宽限期但**仍检查读者临界区**（清流路径：旧帧已不再需要，
 *              但可能有读者刚读到 slot->buf 指针尚未 try_ref）
 */
static void drain_retired(zcs_gop_queue *r, int all)
{
    while (r->retire_n > 0) {
        size_t at = r->retire[r->retire_head].at;

        if (all == 0 && at + ZCS_GOP_QUEUE_RETIRE_GRACE > r->write_idx) {
            break;
        }
        /* 宽限只防常见情况；读者被调度挂起时可能超过宽限。此时不等待、不释放，留到下次提交。
         * 只有销毁路径(all=1)才跳过该检查。 */
        if (all != 1 && ring_readers_in_cs(r)) {
            break;
        }
        zn_buf_unref(r->retire[r->retire_head].buf);
        r->retire[r->retire_head].buf = NULL;
        r->retire_head = (r->retire_head + 1) % ZCS_GOP_QUEUE_RETIRE_CAP;
        r->retire_n--;
    }
}

/** 清空 slot 并把 buf 交给延迟释放（调用方持 mu）。
 *
 * 这里 seq 递增了两次（清空前、清空后），形成一个奇数窗口：任何在这段
 * 期间读到该槽位的读者都会因 seq 为奇数而判定“写入中”，重试或放弃，
 * 不会拿到半清理状态。这是 seqlock 的标准写法。 */
static void retire_slot(zcs_gop_queue *r, zcs_frame_slot *p)
{
    zn_buf *b;

    if (!p || !p->buf) {
        return;
    }
    p->seq++;
    zcs_gop_barrier();
    b = p->buf;
    if (r) {
        if (r->bytes > p->pub.len) {
            r->bytes -= p->pub.len;
        } else {
            r->bytes = 0;
        }
    }
    p->buf = NULL;
    p->pub.data = NULL;
    p->pub.len = 0;
    zcs_gop_barrier();
    p->seq++;
    retire_buf(r, b);
}

/*
 * 写者发布一个帧到槽位（持 mu）。
 *
 * 关键顺序：先把所有数据字段写好，再递增 seq “揭牌”。读者只有看到
 * 偶数且前后一致的 seq 才认为数据有效，因此数据必须先于 seq 可见。
 * 若顺序反了，读者可能看到新 seq 却读到旧数据。
 *
 * 槽位里原有的 old buf（同一槽位被复用、且不是本次的 buf）走 retire 延迟释放。
 */
static void slot_publish(zcs_gop_queue *r, zcs_frame_slot *slot, zn_buf *buf, const zcs_frame *frame,
                         uint32_t dts_ms, uint32_t pts_ms)
{
    zn_buf *old;

    if (!r || !slot || !buf || !frame) {
        return;
    }
    old = (slot->buf == buf) ? NULL : slot->buf;
    {
        size_t old_len = old ? slot->pub.len : 0;
        size_t new_len = zn_buf_len(buf);

        if (new_len >= old_len) {
            r->bytes += new_len - old_len;
        } else if (r->bytes > old_len - new_len) {
            r->bytes -= old_len - new_len;
        } else {
            r->bytes = 0;
        }
    }
    slot->seq++;
    zcs_gop_barrier();
    slot->buf = buf;
    slot->pub.codec = frame->codec;
    slot->pub.track = frame->track;
    slot->pub.keyframe = frame->keyframe;
    slot->pub.config_frame = frame->config_frame;
    slot->pub.drop_able = frame->drop_able;
    slot->pub.dts_ms = dts_ms;
    slot->pub.pts_ms = pts_ms;
    slot->pub.data = (uint8_t *)zn_buf_data(buf);
    slot->pub.len = zn_buf_len(buf);
    zcs_gop_barrier();
    slot->seq++;
    if (old) {
        retire_buf(r, old);
    }
}

static void publish_oldest(zcs_gop_queue *r)
{
    size_t oldest;

    if (!r) {
        return;
    }
    oldest = oldest_idx(r);
    zcs_gop_barrier();
    r->oldest_seq = oldest;
}

static void ring_post_wake(zcs_gop_queue *r)
{
    if (!r || !r->wake_q) {
        return;
    }
    (void)zn_mpsc_push(r->wake_q, r);
}

static void ring_signal_readers(zcs_gop_queue *r, size_t seq, zn_mpsc_queue **snaps, int *snap_n,
                                int snap_cap)
{
    int i;

    if (!r || !snaps || !snap_n) {
        return;
    }
    *snap_n = 0;
    for (i = 0; i < r->reader_count; ++i) {
        zcs_gop_reader *rd = r->readers[i];

        if (!rd) {
            continue;
        }
        rd->wake_pending = 1;
        if (!rd->wake_q) {
            continue;
        }
        if (*snap_n < snap_cap) {
            snaps[(*snap_n)++] = rd->wake_q;
        } else {
            /* 少见：读者数超 snap 容量——持锁 push。 */
            (void)zn_mpsc_push(rd->wake_q, (void *)(uintptr_t)seq);
        }
    }
}

static void ring_flush_reader_wakes(zn_mpsc_queue **snaps, int snap_n, size_t seq)
{
    int i;

    for (i = 0; i < snap_n; ++i) {
        if (snaps[i]) {
            (void)zn_mpsc_push(snaps[i], (void *)(uintptr_t)seq);
        }
    }
}

static int ring_reader_register(zcs_gop_queue *r, zcs_gop_reader *rd)
{
    if (!r || !rd) {
        return -1;
    }
    if (r->reader_count >= r->reader_cap) {
        /* 动态扩容：每次翻倍，最小初始容量 ZCS_GOP_QUEUE_READERS_INIT_CAP */
        int new_cap = r->reader_cap > 0 ? r->reader_cap * 2 : (int)ZCS_GOP_QUEUE_READERS_INIT_CAP;
        zcs_gop_reader **buf =
            (zcs_gop_reader **)realloc(r->readers, (size_t)new_cap * sizeof(r->readers[0]));
        if (!buf) {
            zn_warn("gop_queue: reader registry grow failed (count=%d)", r->reader_count);
            return -1;
        }
        r->readers = buf;
        r->reader_cap = new_cap;
    }
    r->readers[r->reader_count++] = rd;
    return 0;
}

static void ring_reader_unregister(zcs_gop_queue *r, zcs_gop_reader *rd)
{
    int i;

    if (!r || !rd) {
        return;
    }
    for (i = 0; i < r->reader_count; ++i) {
        if (r->readers[i] != rd) {
            continue;
        }
        if (i + 1 < r->reader_count) {
            memmove(&r->readers[i], &r->readers[i + 1],
                    (size_t)(r->reader_count - i - 1) * sizeof(r->readers[0]));
        }
        r->reader_count--;
        return;
    }
}

static size_t beginning_idx(const zcs_gop_queue *r)
{
    if (r->count > 0 && r->write_idx >= r->count) {
        return r->write_idx - r->count;
    }
    /* 空环（含刚 clear 后 write_idx 未回退的情况）：起点即当前写位置，
     * 不能让读者从绝对下标 0 开始扫过一大段已清空的槽位。 */
    return r->write_idx;
}

static size_t oldest_idx(const zcs_gop_queue *r)
{
    if (r->gop_count > 0) {
        return r->gop_start[0];
    }
    if (r->count > 0) {
        return r->write_idx - r->count;
    }
    return r->write_idx;
}

/**
 * 无锁 slot 观察（RCU 式）。
 * @return 1 成功（out_buf 已引用），0 重试/busy，-1 空 slot 跳过。
 */
/*
 * 读者无锁取一个槽位（seqlock 读侧）。
 *
 * 流程与每步的必要性：
 *   in_cs=1            → 告知写者“我在临界区，先别释放 buf”
 *   s1 = seq; 奇数      → 写者正在写，直接放
 *   读 buf/pub         → 读数据
 *   s2 = seq; 与 s1 不同 → 读的过程中被写者改过，数据不可信，放
 *   try_ref(buf)       → 抢一个引用；失败说明 buf 正被回收
 *   再校验一次 seq      → try_ref 期间可能又被覆盖
 *
 * 返回值：1=拿到；0=竞争/暂不可读（调用方可重试）；-1=槽位为空（无数据）。
 * 0 与 -1 必须区分：0 是“再试一次”，-1 是“这里确实没东西”。
 */
static int reader_try_take_slot(zcs_gop_reader *rd, size_t idx, zcs_gop_slot *out, zn_buf **out_buf)
{
    zcs_gop_queue *r;
    zcs_frame_slot *slot;
    uint32_t s1;
    uint32_t s2;
    zn_buf *buf;
    zcs_gop_slot pub;

    if (!rd || !rd->ring || !out || !out_buf) {
        return 0;
    }
    r = rd->ring;
    slot = &r->slots[idx % ZCS_GOP_QUEUE_CAPACITY];

    rd->in_cs = 1;
    zcs_gop_barrier();

    s1 = slot->seq;
    if (s1 & 1u) {
        rd->in_cs = 0;
        return 0;
    }
    zcs_gop_barrier();
    buf = slot->buf;
    pub = slot->pub;
    zcs_gop_barrier();
    s2 = slot->seq;
    if (s1 != s2 || (s2 & 1u)) {
        rd->in_cs = 0;
        return 0;
    }
    if (!buf || pub.len == 0) {
        rd->in_cs = 0;
        return -1;
    }
    if (!zn_buf_try_ref(buf)) {
        rd->in_cs = 0;
        return 0;
    }
    zcs_gop_barrier();
    s2 = slot->seq;
    if (s1 != s2) {
        zn_buf_unref(buf);
        rd->in_cs = 0;
        return 0;
    }
    rd->in_cs = 0;

    pub.data = (uint8_t *)zn_buf_data(buf);
    pub.len = zn_buf_len(buf);
    *out = pub;
    *out_buf = buf;
    return 1;
}

static int frame_mux_fill_one_locked(zcs_gop_reader *rd)
{
    zcs_gop_queue *r;
    size_t oldest;
    zcs_frame_slot *slot;
    zcs_gop_frame_mux_item *it;
    zn_buf *buf;

    if (!rd || !rd->ring) {
        return 0;
    }
    r = rd->ring;

    zb_mutex_lock(r->mu);
    oldest = rd->from_beginning ? beginning_idx(r) : oldest_idx(r);
    if (rd->read_idx < oldest) {
        size_t snap = oldest;

        if (r->gop_count > 0) {
            size_t gop = r->gop_start[r->gop_count - 1];
            if (gop >= oldest) {
                snap = gop;
            }
        }
        rd->read_idx = snap;
        for (size_t i = snap; i < r->write_idx; ++i) {
            if (ring_slot_video_sync(&r->slots[i % ZCS_GOP_QUEUE_CAPACITY])) {
                rd->read_idx = i;
                break;
            }
        }
    }
    if (rd->read_idx >= r->write_idx) {
        zb_mutex_unlock(r->mu);
        return 0;
    }

    slot = &r->slots[rd->read_idx % ZCS_GOP_QUEUE_CAPACITY];
    rd->read_idx++;
    if (!slot->buf || slot->pub.len == 0) {
        zb_mutex_unlock(r->mu);
        return 2;
    }

    buf = zn_buf_ref(slot->buf);
    it = &rd->mux[rd->mux_cnt];
    it->pub = slot->pub;
    it->buf = buf;
    it->pub.data = (uint8_t *)zn_buf_data(buf);
    it->pub.len = zn_buf_len(buf);
    rd->mux_cnt++;
    zb_mutex_unlock(r->mu);
    return 1;
}

static int frame_mux_fill_one(zcs_gop_reader *rd)
{
    zcs_gop_queue *r;
    size_t idx;
    zcs_gop_slot pub;
    zn_buf *buf = NULL;
    zcs_gop_frame_mux_item *it;
    int n;

    if (!rd || !rd->ring) {
        return 0;
    }
    r = rd->ring;

    if (rd->read_idx >= r->write_seq) {
        return 0;
    }
    if (rd->from_beginning || rd->read_idx < r->oldest_seq) {
        return frame_mux_fill_one_locked(rd);
    }

    idx = rd->read_idx;
    n = reader_try_take_slot(rd, idx, &pub, &buf);
    if (n == 1) {
        if (idx < r->oldest_seq) {
            zn_buf_unref(buf);
            return frame_mux_fill_one_locked(rd);
        }
        rd->read_idx = idx + 1;
        it = &rd->mux[rd->mux_cnt];
        it->pub = pub;
        it->buf = buf;
        rd->mux_cnt++;
        return 1;
    }
    if (n == -1) {
        rd->read_idx = idx + 1;
        return 2;
    }
    return frame_mux_fill_one_locked(rd);
}

/*
 * 填充 reader 的 mux 缓冲，供“按 A/V 交织顺序输出”的调用方使用。
 *
 * 返回值语义：正数 = 缓冲里已有条目数；<=0 = 无可读数据。
 * 遇到 n==2（尚未到起始关键帧）时 continue 继续填，而不是返回——
 * 起始前的那几帧就是不该被 mux 出去的。
 */
static int frame_mux_fill(zcs_gop_reader *rd)
{
    if (!rd) {
        return 0;
    }
    while (rd->mux_cnt < ZCS_FRAME_MUX_CAP) {
        int n = frame_mux_fill_one(rd);

        if (n == 2) {
            continue;
        }
        if (n <= 0) {
            return rd->mux_cnt > 0 ? rd->mux_cnt : n;
        }
    }
    return rd->mux_cnt;
}

static void reset_gop(zcs_gop_queue *r)
{
    r->gop_count = 0;
    r->cache_started = 0;
    r->video_key_pos = 0;
    r->has_video = 0;
    r->last_ts_v = 0;
    r->last_ts_a = 0;
}

/**
 * 释放绝对下标 [from, to) 内 slot 持有的帧缓冲（调用方须持有 r->mu）。
 * 读者若已 try_ref 取走的 buf 不受影响（引用计数保证），
 * 之后访问到空 slot 的读者按 pub.len==0 视为跳过。
 */
static void release_slot_range(zcs_gop_queue *r, size_t from, size_t to)
{
    if (to <= from) {
        return;
    }
    if (to - from > ZCS_GOP_QUEUE_CAPACITY) {
        from = to - ZCS_GOP_QUEUE_CAPACITY;
    }
    for (size_t i = from; i < to; ++i) {
        retire_slot(r, &r->slots[i % ZCS_GOP_QUEUE_CAPACITY]);
    }
}

/*
 * 丢掉最旧的一个 GOP，并把它的 slot 缓冲交给 retire。
 *
 * 注意 release_slot_range 里的**立即释放**：早期实现只把 count 减掉、
 * 不动缓冲，导致被裁掉的 GOP 仍占着内存直到槽位被下次写入覆盖——
 * 低码率时表现为内存只涨不降。现在裁剪即刻释放。
 *
 * 回绕处理：abs_idx 用 %CAPACITY 映射到真实槽位，write_idx 回绕后
 * dropped 可能大于 count，必须钳制，否则 count 下溢成巨大值。
 */
static void pop_front_gop(zcs_gop_queue *r)
{
    if (r->gop_count == 0) {
        return;
    }
    size_t drop_begin = r->gop_start[0];
    size_t drop_end = (r->gop_count > 1) ? r->gop_start[1] : r->write_idx;
    /* 时间窗裁剪原先只减 count，slot 仍占着帧缓冲直到被覆盖；这里立即释放。 */
    release_slot_range(r, drop_begin, drop_end);
    if (drop_end > drop_begin) {
        size_t dropped = drop_end - drop_begin;
        /* 按 abs_idx % CAP 回收 slot；write_idx 回绕时 dropped 可能 >512，需钳制 count */
        if (dropped <= r->count) {
            r->count -= dropped;
        } else {
            r->count = 0;
        }
    }
    if (r->gop_count > 1) {
        memmove(r->gop_start, r->gop_start + 1, (r->gop_count - 1) * sizeof(r->gop_start[0]));
        memmove(r->gop_dts, r->gop_dts + 1, (r->gop_count - 1) * sizeof(r->gop_dts[0]));
    }
    r->gop_count--;
    publish_oldest(r);
}

/*
 * 字节超上限时丢最旧 GOP（至少保留当前正在写的那个）。
 *
 * 与时间窗裁剪是两条独立策略，都会触发 pop_front_gop：任何一个阈值超标都
 * 要回收，否则内存会单方向增长（比如低码率但 GOP 极长，时间窗没到、字节已爆）。
 */
static void trim_gops_for_bytes(zcs_gop_queue *r)
{
    if (!r || r->byte_budget == 0) {
        return;
    }
    while (r->gop_count > 1 && r->bytes > r->byte_budget) {
        pop_front_gop(r);
    }
}

static void trim_gops_for_time(zcs_gop_queue *r, uint32_t now_dts)
{
    if (!r || r->cache_ms == 0) {
        return;
    }
    while (r->gop_count > 1) {
        uint32_t oldest = r->gop_dts[0];
        if (now_dts < oldest) {
            break; /* 时间线重置/回绕——保留 */
        }
        if ((now_dts - oldest) <= r->cache_ms) {
            break;
        }
        pop_front_gop(r);
    }
}

/** 新 GOP 边界：更新 key/config 相关游标（含 _video_key_pos）。 */
static void on_new_gop(zcs_gop_queue *r, uint32_t dts_ms)
{
    /* gop_count == 0：首个 GOP；否则在 gop_count-1 处开新段。
     * set_video_config 可能把 cache_started 置 1，此时仍按新 gop 处理。 */
    if (!r->cache_started || r->gop_count == 0) {
        r->cache_started = 1;
        r->gop_start[0] = r->write_idx;
        r->gop_dts[0] = dts_ms;
        r->gop_count = 1;
        publish_oldest(r);
        return;
    }
    size_t cur = r->gop_start[r->gop_count - 1];
    if (r->write_idx > cur) {
        while (r->gop_count >= ZCS_GOP_QUEUE_MAX_GOP ||
               (r->gop_count >= r->target_gops && r->count >= ZCS_GOP_QUEUE_CAPACITY)) {
            pop_front_gop(r);
        }
        trim_gops_for_time(r, dts_ms);
    trim_gops_for_bytes(r);
        r->gop_start[r->gop_count] = r->write_idx;
        r->gop_dts[r->gop_count] = dts_ms;
        r->gop_count++;
    }
}

static zn_err_t ring_commit_frame(zcs_gop_queue *r, const zcs_frame *frame, zn_buf *owned_buf)
{
    zcs_frame_slot *slot;
    uint32_t dts_ms;
    uint32_t pts_ms;
    zn_mpsc_queue *wake_snaps[ZCS_GOP_QUEUE_SIGNAL_SNAP];
    int wake_snap_n = 0;
    size_t seq;

    if (!r || !frame || !owned_buf || zn_buf_len(owned_buf) == 0) {
        return ZN_ERR_INVALID;
    }

    zcs_gop_queue_timeline_from_frame(frame, &dts_ms, &pts_ms);

    zb_mutex_lock(r->mu);
    drain_retired(r, 0);

    if (frame->track == ZCS_TRACK_VIDEO && !frame->config_frame) {
        r->has_video = 1;
    }

    if (!zcs_gop_queue_storable(frame, r->cache_started, r->has_video)) {
        zb_mutex_unlock(r->mu);
        zn_buf_unref(owned_buf);
        return ZN_OK;
    }
    if (zcs_gop_queue_new_gop(frame, r->video_key_pos, r->has_video)) {
        on_new_gop(r, dts_ms);
    }

    while (r->count >= ZCS_GOP_QUEUE_CAPACITY && r->gop_count > 0) {
        pop_front_gop(r);
    }

    slot = &r->slots[r->write_idx % ZCS_GOP_QUEUE_CAPACITY];
    slot_publish(r, slot, owned_buf, frame, dts_ms, pts_ms);

    r->write_idx++;
    if (r->count < ZCS_GOP_QUEUE_CAPACITY) {
        r->count++;
    }

    if (!frame->drop_able) {
        if (frame->track == ZCS_TRACK_VIDEO) {
            r->video_key_pos = zcs_frame_video_gop_marker(frame);
        } else if (!r->has_video) {
            r->video_key_pos = 1;
        }
    }

    if (r->count >= ZCS_GOP_QUEUE_CAPACITY && r->gop_count > 1) {
        pop_front_gop(r);
    }
    trim_gops_for_time(r, dts_ms);
    trim_gops_for_bytes(r);

    zcs_gop_barrier();
    r->write_seq = r->write_idx;
    seq = r->write_seq;
    ring_signal_readers(r, seq, wake_snaps, &wake_snap_n, ZCS_GOP_QUEUE_SIGNAL_SNAP);
    zb_mutex_unlock(r->mu);
    ring_flush_reader_wakes(wake_snaps, wake_snap_n, seq);
    ring_post_wake(r);
    return ZN_OK;
}

/*
 * 写配置到 blob 的非活动缓冲，成功后切换 active（调用方持 mu）。
 *
 * 不做堆分配：指针落在队列本体，跨线程借用永不会因更新而 UAF。
 * 超长配置（> ZCS_GOP_QUEUE_CONFIG_MAX）拒绝并返回 BUFFER_TOO_SMALL：
 * 真实码流的 SPS/PPS/ASC 远小于 1KB，出现超长说明码流异常，宁可丢配置也不截断。
 */
static zn_err_t set_blob(zcs_gop_config_blob *blob, const void *data, size_t len)
{
    unsigned next;

    if (!blob) {
        return ZN_ERR_INVALID;
    }
    if (!data || len == 0) {
        blob->len = 0; /* 保留 active 缓冲内容，仅置空长度 */
        return ZN_OK;
    }
    if (len > sizeof(blob->buf[0])) {
        return ZN_ERR_BUFFER_TOO_SMALL;
    }
    next = blob->active ^ 1u;
    memcpy(blob->buf[next], data, len);
    blob->len = len;
    zcs_gop_barrier();
    blob->active = next;
    return ZN_OK;
}

zcs_gop_queue *zcs_gop_queue_create(void)
{
    zcs_gop_queue *r = (zcs_gop_queue *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->readers = (zcs_gop_reader **)malloc(ZCS_GOP_QUEUE_READERS_INIT_CAP * sizeof(r->readers[0]));
    if (!r->readers) {
        free(r);
        return NULL;
    }
    r->reader_cap = (int)ZCS_GOP_QUEUE_READERS_INIT_CAP;
    r->mu = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!r->mu) {
        free(r->readers);
        free(r);
        return NULL;
    }
    r->wake_q = zn_mpsc_create(ZCS_GOP_QUEUE_WAKE_CAP);
    if (!r->wake_q) {
        zb_mutex_destroy(r->mu);
        free(r->readers);
        free(r);
        return NULL;
    }
    r->target_gops = g_default_target_gops;
    r->cache_ms = g_default_cache_ms;
    r->byte_budget = (unsigned)(g_default_byte_budget > 0xffffffffu ? 0xffffffffu
                                                                  : g_default_byte_budget);
    r->oldest_seq = 0;
    return r;
}

int zcs_gop_queue_drain_wake(zcs_gop_queue *r)
{
    if (!r || !r->wake_q) {
        return 0;
    }
    return zn_mpsc_pop(r->wake_q) != NULL;
}

int zcs_gop_reader_drain_wake(const zcs_gop_reader *rd)
{
    zcs_gop_reader *mut;
    int got = 0;

    if (!rd) {
        return 0;
    }
    if (rd->wake_pending) {
        mut = (zcs_gop_reader *)rd;
        mut->wake_pending = 0;
        got = 1;
    }
    if (rd->wake_q) {
        while (zn_mpsc_pop(rd->wake_q) != NULL) {
            got = 1;
        }
    }
    return got;
}

static void reader_wake_destroy(zcs_gop_reader *rd)
{
    if (!rd || !rd->wake_q) {
        return;
    }
    zn_mpsc_destroy(rd->wake_q);
    rd->wake_q = NULL;
}

static zcs_gop_reader *reader_alloc(zcs_gop_queue *r, size_t read_idx, int from_beginning)
{
    zcs_gop_reader *rd;

    if (!r) {
        return NULL;
    }
    rd = (zcs_gop_reader *)calloc(1, sizeof(*rd));
    if (!rd) {
        return NULL;
    }
    rd->wake_q = zn_mpsc_create(ZCS_GOP_QUEUE_READER_WAKE_CAP);
    if (!rd->wake_q) {
        free(rd);
        return NULL;
    }
    rd->ring = r;
    rd->read_idx = read_idx;
    rd->from_beginning = from_beginning;
    return rd;
}

void zcs_gop_queue_destroy(zcs_gop_queue *r)
{
    if (!r) {
        return;
    }
    /* 销毁路径要求调用方已保证没有读者（source unregister 前读者均已 detach）。 */
    for (size_t i = 0; i < ZCS_GOP_QUEUE_CAPACITY; ++i) {
        free_slot(&r->slots[i]);
    }
    drain_retired(r, 1);
    r->bytes = 0;
    /* 配置为队列内定长双缓冲，随队列一起 free，无需单独释放。 */
    if (r->wake_q) {
        zn_mpsc_destroy(r->wake_q);
    }
    zb_mutex_destroy(r->mu);
    free(r->readers);
    free(r);
}

size_t zcs_gop_queue_pending_count(const zcs_gop_queue *r)
{
    if (!r) {
        return 0;
    }
    size_t n;
    zb_mutex_lock(r->mu);
    n = r->count;
    zb_mutex_unlock(r->mu);
    return n;
}

size_t zcs_gop_queue_gop_count(const zcs_gop_queue *r)
{
    size_t n = 0;

    if (!r) {
        return 0;
    }
    zb_mutex_lock(r->mu);
    n = r->gop_count;
    zb_mutex_unlock(r->mu);
    return n;
}

void zcs_gop_queue_clear(zcs_gop_queue *r)
{
    if (!r) {
        return;
    }
    zb_mutex_lock(r->mu);
    /* 清流不是销毁：读者可能仍在其它线程无锁读槽位，必须走 retire 延迟释放，
     * 不能像早期实现那样 free_slot 立即 unref（那就是 WHEP/WS-FLV/RTSP 播放线程
     * 随机段错误的根因）。 */
    /* 先尽量排空旧 retire 槽位，避免下面 512 个新条目把环挤满而触发“丢最旧”的
     * 立即释放（那样仍可能早释放读者手里的 buf）。 */
    drain_retired(r, 2);
    for (size_t i = 0; i < ZCS_GOP_QUEUE_CAPACITY; ++i) {
        retire_slot(r, &r->slots[i]);
    }
    /* 清空后不会再有人需要这些旧帧，可无视宽限期释放；但仍以 in_cs 为硬性刹车：
     * 有读者正处于“已读到 slot->buf 指针、尚未 try_ref”的窗口时留到下次提交再释放。 */
    drain_retired(r, 2);
    r->bytes = 0;
    r->count = 0;
    reset_gop(r);
    /* 关键：不重置 write_idx / write_seq。已挂着的读者 read_idx 是绝对下标，
     * 归零会让它们“永远等不到数据”而冻结；保持单调后，读者读到的是空槽，
     * 由 oldest_seq 前移到当前写位置自然完成重定位。 */
    zcs_gop_barrier();
    r->oldest_seq = r->write_idx;
    r->video_config.len = 0;
    r->audio_config.len = 0;
    zb_mutex_unlock(r->mu);
}

/*
 * 为一次写入分配缓冲。
 *
 * 注意：**不再复用当前写槽位里已有的 buf**。早期实现会在
 * `slot->buf`（refcnt==1）时直接就地复用，但入站线程拿到该指针后是**在锁外**
 * memcpy 的；如果此刻另一线程（推流重连/linger 过期/webhook close）执行了
 * `zcs_gop_queue_clear`，`retire_slot` 会把这个 buf unref 掉，入站线程随后
 * memcpy 就是写已释放内存 —— 这是崩溃的第三个来源。
 * 现在每次从 buf_pool 取一块，所有权独享；旧的 slot buf 由提交时的
 * slot_publish→retire_buf 延迟释放，天然与 clear 的 retire 语义一致。
 */
zn_buf *zcs_gop_queue_alloc_write(zcs_gop_queue *r, size_t size)
{
    if (!r || size == 0) {
        return NULL;
    }
    return zn_buf_alloc(size);
}

zn_err_t zcs_gop_queue_write(zcs_gop_queue *r, const zcs_frame *frame)
{
    zcs_frame norm;
    zn_buf *buf;
    void *dst;

    if (!r || !frame || !frame->data || frame->size == 0) {
        return ZN_ERR_INVALID;
    }

    norm = *frame;
    zcs_frame_refresh_key_from_es(&norm);
    frame = &norm;

    buf = zcs_gop_queue_alloc_write(r, frame->size);
    if (!buf) {
        return ZN_ERR_NOMEM;
    }
    dst = (void *)zn_buf_data(buf);
    memcpy(dst, frame->data, frame->size);
    zn_buf_set_len(buf, frame->size);
    return ring_commit_frame(r, frame, buf);
}

zn_err_t zcs_gop_queue_write_buf(zcs_gop_queue *r, zn_buf *buf, const zcs_frame *frame)
{
    zcs_frame norm;

    if (!r || !buf || !frame) {
        return ZN_ERR_INVALID;
    }

    norm = *frame;
    zcs_frame_refresh_key_from_es(&norm);
    return ring_commit_frame(r, &norm, buf);
}

zn_err_t zcs_gop_queue_set_video_config(zcs_gop_queue *r, const void *data, size_t len)
{
    if (!r) {
        return ZN_ERR_INVALID;
    }
    zb_mutex_lock(r->mu);
    zn_err_t err = set_blob(&r->video_config, data, len);
    if (err == ZN_OK) {
        r->cache_started = 1;
        r->has_video = 1;
    }
    zb_mutex_unlock(r->mu);
    return err;
}

zn_err_t zcs_gop_queue_set_audio_config(zcs_gop_queue *r, const void *data, size_t len)
{
    if (!r) {
        return ZN_ERR_INVALID;
    }
    zb_mutex_lock(r->mu);
    zn_err_t err = set_blob(&r->audio_config, data, len);
    zb_mutex_unlock(r->mu);
    return err;
}

const uint8_t *zcs_gop_queue_video_config(const zcs_gop_queue *r, size_t *len)
{
    if (!r) {
        return NULL;
    }
    zb_mutex_lock(r->mu);
    if (len) {
        *len = r->video_config.len;
    }
    /* 指向队列内定长缓冲：队列存活期间始终有效，跨线程借用安全。 */
    const uint8_t *p = r->video_config.len > 0 ? r->video_config.buf[r->video_config.active]
                                               : NULL;
    zb_mutex_unlock(r->mu);
    return p;
}

const uint8_t *zcs_gop_queue_audio_config(const zcs_gop_queue *r, size_t *len)
{
    if (!r) {
        return NULL;
    }
    zb_mutex_lock(r->mu);
    if (len) {
        *len = r->audio_config.len;
    }
    const uint8_t *p = r->audio_config.len > 0 ? r->audio_config.buf[r->audio_config.active]
                                               : NULL;
    zb_mutex_unlock(r->mu);
    return p;
}

static size_t gop_queue_copy_blob(zcs_gop_queue *r, const zcs_gop_config_blob *blob, uint8_t *buf,
                                  size_t cap)
{
    size_t n = 0;

    if (!r || !blob || !buf || !cap) {
        return 0;
    }
    zb_mutex_lock(r->mu);
    n = blob->len;
    if (n > 0 && n <= cap) {
        memcpy(buf, blob->buf[blob->active], n);
    } else {
        n = 0;
    }
    zb_mutex_unlock(r->mu);
    return n;
}

size_t zcs_gop_queue_copy_video_config(zcs_gop_queue *r, uint8_t *buf, size_t cap)
{
    if (!r) {
        return 0;
    }
    return gop_queue_copy_blob(r, &r->video_config, buf, cap);
}

size_t zcs_gop_queue_copy_audio_config(zcs_gop_queue *r, uint8_t *buf, size_t cap)
{
    if (!r) {
        return 0;
    }
    return gop_queue_copy_blob(r, &r->audio_config, buf, cap);
}

zcs_codec_id zcs_gop_queue_audio_codec(const zcs_gop_queue *r)
{
    zcs_codec_id c = ZCS_CODEC_INVALID;

    if (!r) {
        return ZCS_CODEC_INVALID;
    }
    zb_mutex_lock(r->mu);
    if (r->audio_config.len > 0) {
        c = zcs_flv_tag_audio_codec(r->audio_config.buf[r->audio_config.active],
                                    r->audio_config.len);
    }
    zb_mutex_unlock(r->mu);
    return c;
}

zcs_gop_reader *zcs_gop_reader_attach_beginning(zcs_gop_queue *r)
{
    zcs_gop_reader *rd;

    if (!r) {
        return NULL;
    }
    zb_mutex_lock(r->mu);
    rd = reader_alloc(r, beginning_idx(r), 1);
    if (!rd || ring_reader_register(r, rd) != 0) {
        zb_mutex_unlock(r->mu);
        reader_wake_destroy(rd);
        free(rd);
        return NULL;
    }
    zb_mutex_unlock(r->mu);
    return rd;
}

zcs_gop_reader *zcs_gop_reader_attach(zcs_gop_queue *r)
{
    zcs_gop_reader *rd;

    if (!r) {
        return NULL;
    }
    zb_mutex_lock(r->mu);
    rd = reader_alloc(r, oldest_idx(r), 0);
    if (!rd || ring_reader_register(r, rd) != 0) {
        zb_mutex_unlock(r->mu);
        reader_wake_destroy(rd);
        free(rd);
        return NULL;
    }
    zb_mutex_unlock(r->mu);
    return rd;
}

int zcs_gop_reader_count(const zcs_gop_queue *r)
{
    int n = 0;

    if (!r) {
        return 0;
    }
    zb_mutex_lock(r->mu);
    n = r->reader_count;
    zb_mutex_unlock(r->mu);
    return n;
}

void zcs_gop_reader_detach(zcs_gop_reader *rd)
{
    zcs_gop_queue *r;

    if (!rd) {
        return;
    }
    r = rd->ring;
    if (r) {
        zb_mutex_lock(r->mu);
        ring_reader_unregister(r, rd);
        zb_mutex_unlock(r->mu);
    }
    frame_mux_clear(rd);
    reader_unref_held(rd);
    reader_wake_destroy(rd);
    free(rd);
}

int zcs_gop_reader_slot_at_read(const zcs_gop_reader *rd, zcs_gop_slot *out)
{
    zcs_gop_queue *r;
    const zcs_frame_slot *slot;

    if (!rd || !rd->ring || !out) {
        return 0;
    }
    r = rd->ring;
    if (rd->read_idx >= r->write_seq) {
        return 0;
    }
    zb_mutex_lock(r->mu);
    if (rd->read_idx >= r->write_idx) {
        zb_mutex_unlock(r->mu);
        return 0;
    }
    slot = &r->slots[rd->read_idx % ZCS_GOP_QUEUE_CAPACITY];
    *out = slot->pub;
    zb_mutex_unlock(r->mu);
    return out->data && out->len > 0;
}

int zcs_gop_reader_at_decode_start(const zcs_gop_reader *rd)
{
    zcs_gop_queue *r;
    int ok = 0;

    if (!rd || !rd->ring) {
        return 0;
    }
    r = rd->ring;
    if (rd->read_idx >= r->write_seq) {
        return 0;
    }
    zb_mutex_lock(r->mu);
    if (rd->read_idx < r->write_idx) {
        ok = zcs_gop_slot_is_decode_start(&r->slots[rd->read_idx % ZCS_GOP_QUEUE_CAPACITY].pub);
    }
    zb_mutex_unlock(r->mu);
    return ok;
}

size_t zcs_gop_reader_lag(const zcs_gop_reader *rd)
{
    size_t w;
    size_t lag = 0;

    if (!rd || !rd->ring) {
        return 0;
    }
    w = rd->ring->write_seq;
    if (w > rd->read_idx) {
        lag = w - rd->read_idx;
    }
    return lag;
}

size_t zcs_gop_queue_max_reader_lag(const zcs_gop_queue *r)
{
    size_t max_lag = 0;
    int i;

    if (!r) {
        return 0;
    }
    zb_mutex_lock(r->mu);
    for (i = 0; i < r->reader_count; ++i) {
        const zcs_gop_reader *rd = r->readers[i];
        size_t lag;
        size_t w;

        if (!rd) {
            continue;
        }
        w = r->write_seq;
        lag = (w > rd->read_idx) ? (w - rd->read_idx) : 0;
        if (lag > max_lag) {
            max_lag = lag;
        }
    }
    zb_mutex_unlock(r->mu);
    return max_lag;
}

int zcs_gop_reader_read(zcs_gop_reader *rd, zcs_gop_slot *out)
{
    zcs_gop_queue *r;

    if (!rd || !out) {
        return -1;
    }
    r = rd->ring;
    if (!r) {
        return 0;
    }

    for (;;) {
        size_t w;
        size_t oldest;
        size_t idx;
        zcs_frame_slot *slot;
        zn_buf *buf;
        zcs_gop_slot pub;
        int n;

        if (rd->read_idx >= r->write_seq) {
            return 0;
        }

        /* 直播快路径：RCU 式观察，不持 mu。 */
        if (!rd->from_beginning && rd->read_idx >= r->oldest_seq) {
            idx = rd->read_idx;
            n = reader_try_take_slot(rd, idx, &pub, &buf);
            if (n == 1) {
                if (idx < r->oldest_seq) {
                    zn_buf_unref(buf);
                } else {
                    rd->read_idx = idx + 1;
                    reader_unref_held(rd);
                    rd->held_ref = buf;
                    *out = pub;
                    return 1;
                }
            } else if (n == -1) {
                rd->read_idx = idx + 1;
                continue;
            }
            /* busy/竞态 → 持锁路径 */
        }

        zb_mutex_lock(r->mu);
        oldest = rd->from_beginning ? beginning_idx(r) : oldest_idx(r);
        if (rd->read_idx < oldest) {
            rd->read_idx = oldest;
        }

        w = r->write_idx;
        if (rd->read_idx >= w) {
            zb_mutex_unlock(r->mu);
            return 0;
        }

        idx = rd->read_idx++;
        slot = &r->slots[idx % ZCS_GOP_QUEUE_CAPACITY];
        if (!slot->buf || slot->pub.len == 0) {
            zb_mutex_unlock(r->mu);
            continue;
        }

        buf = zn_buf_ref(slot->buf);
        out->codec = slot->pub.codec;
        out->track = slot->pub.track;
        out->dts_ms = slot->pub.dts_ms;
        out->pts_ms = slot->pub.pts_ms;
        out->keyframe = slot->pub.keyframe;
        out->config_frame = slot->pub.config_frame;
        out->drop_able = slot->pub.drop_able;
        out->len = slot->pub.len;
        zb_mutex_unlock(r->mu);
        reader_unref_held(rd);
        rd->held_ref = buf;
        out->data = (uint8_t *)zn_buf_data(buf);
        return 1;
    }
}

int zcs_gop_reader_peek_muxed(zcs_gop_reader *rd, zcs_gop_slot *out, uint32_t max_skew_ms)
{
    int best;

    if (!rd || !out) {
        return -1;
    }
    if (max_skew_ms == 0) {
        max_skew_ms = ZCS_GOP_QUEUE_MUX_MAX_SKEW_MS;
    }

    if (rd->mux_cnt == 0) {
        int n = frame_mux_fill(rd);
        if (n <= 0) {
            return n;
        }
    }

    best = frame_mux_pick_best(rd, max_skew_ms);
    if (best < 0) {
        frame_mux_clear(rd);
        return 0;
    }

    {
        const zcs_gop_frame_mux_item *it = &rd->mux[best];
        *out = it->pub;
        out->data = it->pub.data;
        out->len = it->pub.len;
    }
    return 1;
}

int zcs_gop_reader_read_muxed(zcs_gop_reader *rd, zcs_gop_slot *out, uint32_t max_skew_ms)
{
    if (!rd || !out) {
        return -1;
    }
    if (max_skew_ms == 0) {
        max_skew_ms = ZCS_GOP_QUEUE_MUX_MAX_SKEW_MS;
    }

    if (rd->mux_cnt == 0) {
        int n = frame_mux_fill(rd);
        if (n <= 0) {
            return n;
        }
    }

    int best = frame_mux_pick_best(rd, max_skew_ms);
    if (best < 0) {
        frame_mux_clear(rd);
        return 0;
    }
    zcs_gop_frame_mux_item *it = &rd->mux[best];

    *out = it->pub;
    out->data = it->pub.data;
    out->len = it->pub.len;
    reader_unref_held(rd);
    rd->held_ref = it->buf;
    it->buf = NULL;

    for (int i = best + 1; i < rd->mux_cnt; ++i) {
        rd->mux[i - 1] = rd->mux[i];
    }
    rd->mux_cnt--;
    rd->mux[rd->mux_cnt].buf = NULL;
    return 1;
}
