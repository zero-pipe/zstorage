#ifndef ZMS_SRC_CORE_GOP_QUEUE_INTERNAL_H
#define ZMS_SRC_CORE_GOP_QUEUE_INTERNAL_H

/**
 * @file queue_internal.h
 * @brief 直播 GOP 队列及其读者的私有布局（跨文件共享）。
 *        队列/读者核心（gop_queue.c）与读者定位模块（gop_reader_seek.c）共享。
 *        非公开 API。
 */
#include "zms/core/gop/queue.h"
#include "zms/core/gop/limits.h"
#include "zms/media/media_limits.h"
#include "zb/os/sync.h"
#include "zn/util/buf.h"
#include "zn/util/mpsc.h"
#include <stddef.h>
#include <stdint.h>

#define ZMS_FRAME_MUX_CAP 24

/* 延迟释放宽限（帧数）。读者无锁临界区是纳秒级，16 帧远大于它。 */
#define ZMS_GOP_QUEUE_RETIRE_GRACE 16u

/* 待释放环容量：宽限期内条目 <= 单次裁剪最多 CAP + 宽限帧数，取 2*CAP。 */
#define ZMS_GOP_QUEUE_RETIRE_CAP (2u * ZMS_GOP_QUEUE_CAPACITY)

/* 视频/音频 config 的定长容量（FLV sequence header / AVCC / HVCC / AV1C，实测远小于此值）。 */
#define ZMS_GOP_QUEUE_CONFIG_MAX 2048u

/*
 * 配置使用「队列内定长双缓冲」而不是堆指针：
 *   - set_video_config/set_audio_config 写非活动缓冲再切换 active，读侧拿到的指针
 *     永远落在队列本体内，生命周期 = 队列本身，**不会**因配置更新/清流被释放；
 *   - 旧实现返回堆指针，跨线程借用期间一旦 set/clear（每次重连、SPS 重建）就会
 *     UAF —— 这正是 WHEP/WS-FLV 播放线程随机段错误的第二个来源。
 *   - 双缓冲让「刚切走的那份」在下一次切换前仍可安全读取，避免撕读。
 */
typedef struct zms_gop_config_blob {
    uint8_t buf[2][ZMS_GOP_QUEUE_CONFIG_MAX];
    size_t len;
    unsigned active; /* 当前生效的缓冲下标：0/1 */
} zms_gop_config_blob;

/** ring / reader 暂存：公开 gop_slot 视图 + 持有的 zn_buf 引用。 */
typedef struct zms_frame_slot {
    zms_gop_slot pub;
    zn_buf *buf;
    /** Seqlock：偶数=稳定，奇数=写者更新中（RCU 式读者）。 */
    volatile uint32_t seq;
} zms_frame_slot;

/** 别名：reader A/V 交错暂存与 ring slot 同布局。 */
typedef zms_frame_slot zms_gop_frame_mux_item;

struct zms_gop_queue {
    zms_frame_slot slots[ZMS_GOP_QUEUE_CAPACITY];
    size_t write_idx;
    volatile size_t write_seq;
    /** 逻辑最旧绝对下标；无锁读者落后时 snap 对齐。 */
    volatile size_t oldest_seq;
    size_t count;
    size_t gop_start[ZMS_GOP_QUEUE_MAX_GOP];
    uint32_t gop_dts[ZMS_GOP_QUEUE_MAX_GOP]; /* 各 GOP 起点 dts_ms */
    size_t gop_count;
    unsigned target_gops; /* 压力下保留 GOP 数；默认 TARGET_GOPS */
    unsigned cache_ms;    /* 0=关闭时间裁剪；否则丢弃超此时长的 GOP */
    /** 环内帧数据字节数（发布加、清 slot 减）；byte_budget=0 时不限。 */
    size_t bytes;
    unsigned byte_budget;
    int cache_started;
    uint32_t last_ts_v;
    uint32_t last_ts_a;
    zms_gop_config_blob video_config;
    zms_gop_config_blob audio_config;
    int video_key_pos;
    int has_video;
    zms_gop_reader **readers; /* 动态数组，按需扩容 */
    int reader_count;
    int reader_cap;
    zb_mutex *mu;
    zn_mpsc_queue *wake_q;
    /* 延迟释放环：被覆盖或裁剪的 buf 在 ZMS_GOP_QUEUE_RETIRE_GRACE 帧后才 unref。 */
    struct {
        zn_buf *buf;
        size_t at; /* 入队时的 write_idx */
    } retire[ZMS_GOP_QUEUE_RETIRE_CAP];
    size_t retire_head;
    size_t retire_n;
};

struct zms_gop_reader {
    zms_gop_queue *ring;
    size_t read_idx;
    int from_beginning;
    volatile int wake_pending;
    /** 无锁临界区观察 slot 指针期间非零。 */
    volatile int in_cs;
    zn_mpsc_queue *wake_q;
    zn_buf *held_ref;
    zms_gop_frame_mux_item mux[ZMS_FRAME_MUX_CAP];
    int mux_cnt;
};

/* 核心与 seek 模块共享。 */
int ring_slot_video_sync(const zms_frame_slot *slot);
void frame_mux_clear(zms_gop_reader *rd);

#endif /* ZMS_SRC_CORE_GOP_QUEUE_INTERNAL_H */
