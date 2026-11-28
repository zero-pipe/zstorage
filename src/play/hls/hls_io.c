/**
 * @file hls_io.c
 * @brief HLS 分片/播放列表异步文件写：单 worker + 全局 FIFO，避免阻塞 poller。
 */
#include "play/hls/hls_io.h"
#include "zn/thread/thread_pool.h"
#include "zb/os/platform.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct hls_io_job {
    struct hls_io_job *next;
    char path[768];
    char tmp[776];
    void *data;
    size_t len;
} hls_io_job;

static zn_thread_pool *g_pool;
static zb_mutex *g_mu;
static hls_io_job *g_head;
static hls_io_job *g_tail;
static int g_busy;
static int g_stopping;

static void hls_io_write_job(hls_io_job *job)
{
    FILE *fp;

    if (!job)
        return;
    fp = fopen(job->tmp, "wb");
    if (!fp)
        return;
    if (fwrite(job->data, 1, job->len, fp) != job->len) {
        fclose(fp);
        remove(job->tmp);
        return;
    }
    fclose(fp);
    remove(job->path);
    (void)rename(job->tmp, job->path);
}

static void hls_io_free_job(hls_io_job *job)
{
    if (!job)
        return;
    free(job->data);
    free(job);
}

static void hls_io_drain(void *user)
{
    (void)user;
    for (;;) {
        hls_io_job *job;
        zb_mutex_lock(g_mu);
        job = g_head;
        if (job)
            g_head = job->next;
        if (!g_head)
            g_tail = NULL;
        if (!job)
            g_busy = 0;
        zb_mutex_unlock(g_mu);

        if (!job)
            break;
        hls_io_write_job(job);
        hls_io_free_job(job);
    }
}

void zcs_hls_io_init(void)
{
    zn_thread_pool_opts_t opts;

    if (g_pool)
        return;
    g_mu = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!g_mu)
        return;
    memset(&opts, 0, sizeof(opts));
    opts.thread_count = 1;
    opts.priority = ZB_THREAD_PRIO_LOW;
    opts.auto_start = 1;
    g_pool = zn_thread_pool_create(&opts);
    if (!g_pool) {
        zb_mutex_destroy(g_mu);
        g_mu = NULL;
        zn_warn("hls io thread create failed, segment save falls back to caller");
    }
}

void zcs_hls_io_fini(void)
{
    int i;

    if (!g_mu) {
        return;
    }
    zb_mutex_lock(g_mu);
    g_stopping = 1;
    zb_mutex_unlock(g_mu);

    for (i = 0; i < 1000; ++i) {
        int busy;
        zb_mutex_lock(g_mu);
        busy = g_busy;
        zb_mutex_unlock(g_mu);
        if (!busy)
            break;
        zb_sleep_ms(5);
    }

    /* 线程池/锁保留到进程结束：server 可反复 start/stop，避免销毁瞬间与
     * 正在入队的写任务竞争。队列已在本函数前等待 busy 归零。 */
    zb_mutex_lock(g_mu);
    while (g_head) {
        hls_io_job *job = g_head;
        g_head = job->next;
        hls_io_free_job(job);
    }
    g_tail = NULL;
    g_stopping = 0;
    zb_mutex_unlock(g_mu);
}

void zcs_hls_io_enqueue(const char *save_dir, const char *name, const void *data, size_t len)
{
    hls_io_job *job;
    int kick = 0;
    int n;

    if (!save_dir || !save_dir[0] || !name || !name[0] || !data || len == 0 ||
        strstr(name, ".."))
        return;

    job = (hls_io_job *)calloc(1, sizeof(*job));
    if (!job)
        return;
    n = snprintf(job->path, sizeof(job->path), "%s/%s", save_dir, name);
    if (n <= 0 || (size_t)n >= sizeof(job->path)) {
        free(job);
        return;
    }
    n = snprintf(job->tmp, sizeof(job->tmp), "%s.tmp", job->path);
    if (n <= 0 || (size_t)n >= sizeof(job->tmp)) {
        free(job);
        return;
    }
    job->data = malloc(len);
    if (!job->data) {
        free(job);
        return;
    }
    memcpy(job->data, data, len);
    job->len = len;

    /* 池不可用/已停止：不在调用线程（poller）上同步写盘。
     * HLS 分片是缓存、可再生，丢弃并告警比卡住事件循环划算。 */
    if (!g_mu || !g_pool || g_stopping) {
        zn_warn("hls io unavailable, drop segment %s (%u bytes)", job->path, (unsigned)job->len);
        hls_io_free_job(job);
        return;
    }

    zb_mutex_lock(g_mu);
    if (g_stopping) {
        zb_mutex_unlock(g_mu);
        zn_warn("hls io stopping, drop segment %s (%u bytes)", job->path, (unsigned)job->len);
        hls_io_free_job(job);
        return;
    }
    if (g_tail)
        g_tail->next = job;
    else
        g_head = job;
    g_tail = job;
    if (!g_busy) {
        g_busy = 1;
        kick = 1;
    }
    zb_mutex_unlock(g_mu);

    if (kick && zn_thread_pool_async(g_pool, hls_io_drain, NULL, 0) != ZN_OK) {
        /* 投递失败：当前线程直接排空，保持顺序与可靠性。 */
        hls_io_drain(NULL);
    }
}
