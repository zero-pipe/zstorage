/**
 * @file test_mp4_recorder_async.c
 * @brief 直播 MP4 录制：poller 同步写盘 + 后台异步关文件（.tmp 改名）。
 *
 * 验证：录制产生文件、关闭后正式名存在、无残留 .tmp、线程池收尾无泄漏。
 */
#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/frame.h"
#include "zcs/util/thread_pool.h"
#include "zn/poller/poller.h"
#include "zb/os/thread.h"
#include "zb/os/platform.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
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

static volatile int g_stop;

static void poller_main(void *user)
{
    zn_poller_run((zn_poller *)user, &g_stop);
}

/* 最小 H.264 SPS/PPS + avcC 记录（标清，够 mov writer 建轨）。 */
static const uint8_t k_sps[] = { 0x67, 0x42, 0x00, 0x1e, 0x95, 0xa8, 0x14, 0x01, 0x6e, 0x9b,
                                 0x80, 0x80, 0x80, 0xa0 };
static const uint8_t k_pps[] = { 0x68, 0xce, 0x3c, 0x80 };

static size_t build_avcc(uint8_t *out, size_t cap)
{
    size_t n = 0;

    if (cap < 13 + sizeof(k_sps) + sizeof(k_pps)) {
        return 0;
    }
    /* FLV 视频配置帧头：0x17 = keyframe + AVC，0x00 = sequence header。 */
    out[n++] = 0x17;
    out[n++] = 0x00;
    out[n++] = 1; /* configurationVersion */
    out[n++] = 0x42; /* AVCProfileIndication */
    out[n++] = 0x00; /* profile_compatibility */
    out[n++] = 0x1e; /* AVCLevelIndication */
    out[n++] = 0xff; /* lengthSizeMinusOne=3 */
    out[n++] = 0xe1; /* numOfSPS=1 */
    out[n++] = 0;
    out[n++] = (uint8_t)sizeof(k_sps);
    memcpy(out + n, k_sps, sizeof(k_sps));
    n += sizeof(k_sps);
    out[n++] = 1; /* numOfPPS */
    out[n++] = 0;
    out[n++] = (uint8_t)sizeof(k_pps);
    memcpy(out + n, k_pps, sizeof(k_pps));
    n += sizeof(k_pps);
    return n;
}

/* annexb: 4 字节起始码 + IDR NAL + 填充载荷。 */
static size_t build_idr(uint8_t *out, size_t cap)
{
    size_t n = 0;
    size_t i;

    if (cap < 2048) {
        return 0;
    }
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 0;
    out[n++] = 1;
    out[n++] = 0x65; /* IDR slice */
    for (i = 0; i < 2000; ++i) {
        out[n++] = (uint8_t)(i * 7u);
    }
    return n;
}

/* 统计目录下正式 mp4 与 .tmp 残留数量；找到的正式文件顺带删除。 */
static int scan_mp4_dir(const char *dir, int *final_count, int *tmp_count)
{
#if defined(_WIN32)
    struct _finddata_t fd;
    intptr_t h;
    char pat[600];

    snprintf(pat, sizeof(pat), "%s\\*.mp4", dir);
    h = _findfirst(pat, &fd);
    if (h == -1) {
        return -1;
    }
    do {
        if (fd.name[0] == '.') {
            ++(*tmp_count); /* .<name>.mp4 为写入中的临时名 */
        } else {
            char full[700];
            ++(*final_count);
            snprintf(full, sizeof(full), "%s\\%s", dir, fd.name);
            remove(full);
        }
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    struct dirent *e;

    if (!d) {
        return -1;
    }
    while ((e = readdir(d)) != NULL) {
        if (!strstr(e->d_name, ".mp4")) {
            continue;
        }
        if (e->d_name[0] == '.') {
            ++(*tmp_count);
        } else {
            char full[700];
            ++(*final_count);
            snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
            remove(full);
        }
    }
    closedir(d);
    return 0;
#endif
}

int main(void)
{
    static zcs_media_source src;
    zcs_record_config rcfg;
    zcs_gop_queue *q;
    zn_poller *p;
    zb_thread *th;
    uint8_t avcc[64];
    uint8_t idr[2100];
    size_t avcc_len;
    size_t idr_len;
    const char *root = "./zcs_test_rec_async";
    time_t now;
    struct tm tm_now;
    char date[32];
    char dir[512];
    int final_count = 0;
    int tmp_count = 0;
    int i;

#if defined(_WIN32)
    _mkdir(root);
#else
    mkdir(root, 0755);
#endif

    memset(&rcfg, 0, sizeof(rcfg));
    snprintf(rcfg.root, sizeof(rcfg.root), "%s", root);
    zcs_mp4_recorder_configure(&rcfg);

    zcs_vod_thread_pool_init(1);

    p = zn_poller_create();
    CHECK(p != NULL);
    th = zb_thread_create(poller_main, p);
    CHECK(th != NULL);
    for (i = 0; i < 200 && !zn_poller_is_running(p); ++i) {
        zb_sleep_ms(1);
    }
    CHECK(zn_poller_is_running(p));

    q = zcs_gop_queue_create();
    CHECK(q != NULL);
    avcc_len = build_avcc(avcc, sizeof(avcc));
    CHECK(avcc_len > 0);
    CHECK(zcs_gop_queue_set_video_config(q, avcc, avcc_len) == ZN_OK);

    memset(&src, 0, sizeof(src));
    src.gop_queue = q;
    src.enable_mp4 = 1;
    src.has_video = 1;
    src.video.codec = ZCS_CODEC_H264;
    src.video.width = 640;
    src.video.height = 480;
    snprintf(src.app, sizeof(src.app), "%s", "live");
    snprintf(src.stream, sizeof(src.stream), "%s", "rec");

    CHECK(zcs_mp4_recorder_start(&src, p) == ZN_OK);

    idr_len = build_idr(idr, sizeof(idr));
    CHECK(idr_len > 0);
    for (i = 0; i < 20; ++i) {
        zcs_frame f;
        memset(&f, 0, sizeof(f));
        f.data = idr;
        f.size = idr_len;
        f.dts_ms = (uint64_t)i * 40u;
        f.pts_ms = f.dts_ms;
        f.codec = ZCS_CODEC_H264;
        f.track = ZCS_TRACK_VIDEO;
        f.keyframe = 1;
        (void)zcs_gop_queue_write(q, &f);
        zb_sleep_ms(20);
    }

    /* 停推触发异步关文件。 */
    zcs_mp4_recorder_stop(&src);
    zb_sleep_ms(200);

    g_stop = 1;
    zn_poller_wake(p);
    CHECK(zb_thread_join(th) == ZB_OK);
    zb_thread_destroy(th);
    zn_poller_destroy(p);
    /* 等后台关闭任务跑完（worker 会先执行完队列再退出）。 */
    zcs_vod_thread_pool_fini();

    now = time(NULL);
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(date, sizeof(date), "%Y-%m-%d", &tm_now);
    snprintf(dir, sizeof(dir), "%s/rec/%s", root, date);

    /* 录制必须落地：至少一个正式文件，且没有 .tmp 残留。 */
    scan_mp4_dir(dir, &final_count, &tmp_count);
    zcs_gop_queue_destroy(q);

    if (final_count < 1) {
        fprintf(stderr, "no finished mp4 under %s\n", dir);
        return 1;
    }
    if (tmp_count != 0) {
        fprintf(stderr, "%d leftover .tmp mp4 under %s\n", tmp_count, dir);
        return 1;
    }
    printf("test_mp4_recorder_async: OK (files=%d)\n", final_count);
    return 0;
}
