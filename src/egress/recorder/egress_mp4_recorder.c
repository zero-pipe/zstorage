/**
 * @file egress_mp4_recorder.c
 * @brief 直播 MP4 滚动录像：gop_reader → annexb_to_mp4 → mov_writer。
 *
 * 路径规则见 egress_mp4_recorder.h（{root}/{stream}/{date}/{HHmmss}.mp4）。
 */
#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include "zcs/ops/api/webhook/webhook_client.h"
#include "zcs/util/thread_pool.h"
#include "zcs/core/event.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/codec/h264/h264_config.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h264/h264_over_rtmp.h"
#include "zcs/media/codec/h265/h265_config.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/codec/aac/aac_config.h"
#include "zcs/media/codec/aac/aac_over_rtmp.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/media/clock.h"
#include "zcs/core/gop/queue.h"
#include "media/container/mp4/mov_file_buffer.h"
#include "mov-writer.h"
#include "mov-format.h"
#include "zn/util/log.h"
#include "zb/os/atomic.h"
#include "zn/poller/poller.h"
#include "zn/util/timer.h"
#include "zb/os/sync.h"
#include "zn/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#define zcs_mp4_mkdir(p) _mkdir(p)
#define zcs_mp4_access(p) _access((p), 0)
#else
#include <unistd.h>
#include <dirent.h>
#include <sys/statvfs.h>
#define zcs_mp4_mkdir(p) mkdir((p), 0755)
#define zcs_mp4_access(p) access((p), F_OK)
#endif

#define ZCS_MP4_REC_TICK_MS 20
/* 每 tick 最多消费 32 帧：既能让泵跟上码率，又不会让单次 tick 长时间占用
 * poller，拖慢同线程的其它会话。 */
#define ZCS_MP4_REC_TICK_FRAMES 32
#define ZCS_MP4_PATH_MAX 768

typedef struct zcs_mp4_recorder {
    zcs_media_source *src;
    zcs_gop_reader *reader;
    zb_mutex *mu;
    zn_timer *timer;
    zn_poller *timer_poller;
    /** 停止只认领一次：stop 可能从任意 poller 触发。 */
    volatile int32_t stop_claimed;
    int closing;

    mov_writer_t *mov;
    FILE *fp;
    char file_path[ZCS_MP4_PATH_MAX];
    /** 写入中的临时名（.<file_name>），关闭后 rename 为正式名。 */
    char tmp_path[ZCS_MP4_PATH_MAX];
    char folder[ZCS_MP4_PATH_MAX];
    char file_name[256];
    time_t start_unix;
    uint32_t start_dts_ms;
    uint32_t last_dts_ms;
    int have_start_dts;
    int video_track;
    int audio_track;
    int video_armed;
    int sent_video_cfg;
    int sent_audio_cfg;

    zcs_avc_config avc;
    zcs_hevc_config hevc;
    zcs_aac_config aac;
    int have_aac;
    zcs_codec_id video_codec;

    uint8_t *mux_buf;
    size_t mux_buf_cap;
    zcs_mux_av_timeline mux_av;
    int max_second;
} zcs_mp4_recorder;

static char g_record_root[ZCS_CFG_PATH_MAX];
static int g_mp4_max_second = 180;
/** 录像根目录总大小上限（字节）；0=不限。 */
static uint64_t g_record_max_total_bytes;
/** 录制前磁盘剩余阈值（字节）；0=不检查。 */
static uint64_t g_record_free_space_bytes;

void zcs_mp4_recorder_configure(const zcs_record_config *rec)
{
    if (!rec) {
        return;
    }
    if (rec->root[0]) {
        strncpy(g_record_root, rec->root, sizeof(g_record_root) - 1);
        g_record_root[sizeof(g_record_root) - 1] = '\0';
    } else {
        strncpy(g_record_root, "./www/record", sizeof(g_record_root) - 1);
    }
    g_mp4_max_second = rec->mp4_max_second > 0 ? rec->mp4_max_second : 180;
    g_record_max_total_bytes = rec->max_total_mb > 0 ? (uint64_t)rec->max_total_mb * 1024u * 1024u : 0;
    g_record_free_space_bytes =
        rec->free_space_check_mb > 0 ? (uint64_t)rec->free_space_check_mb * 1024u * 1024u : 0;
}

static int rec_enforce_total_limit(void);

int zcs_mp4_recorder_enforce_retention(void)
{
    return rec_enforce_total_limit();
}

const char *zcs_mp4_recorder_root(void)
{
    return g_record_root[0] ? g_record_root : "./www/record";
}

static void path_norm_slashes(char *p)
{
    if (!p) {
        return;
    }
    for (; *p; ++p) {
        if (*p == '\\') {
            *p = '/';
        }
    }
}

/*
 * 目录穿越校验：拒绝任何 ".." 路径段（作为完整段出现，而非子串）。返回 1
 * 表示含危险段。file_path 可能源自 HTTP 请求，直接拿来做 VOD 读取前必须
 * 过这一关，防止读到录像目录之外的任意文件。
 */
static int path_has_dotdot(const char *p)
{
    const char *s;

    if (!p) {
        return 1;
    }
    for (s = p; *s; ++s) {
        if (s[0] == '.' && s[1] == '.' &&
            (s == p || s[-1] == '/' || s[-1] == '\\') &&
            (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
            return 1;
        }
    }
    return 0;
}

/* ── 录制磁盘治理 ────────────────────────────────────────────────────────
 * 所有函数在 IO 线程/同步路径调用，不涉及 poller。
 * 策略：总量超限时按 mtime 从最旧删起；写入前检查磁盘剩余。 */
typedef struct rec_file_ent {
    char path[ZCS_MP4_PATH_MAX];
    time_t mtime;
    uint64_t size;
} rec_file_ent;

#if !defined(_WIN32)
static void rec_collect_files(const char *dir, rec_file_ent **arr, size_t *cap, size_t *count)
{
    DIR *dp = opendir(dir);
    struct dirent *ent;

    if (!dp) {
        return;
    }
    while ((ent = readdir(dp)) != NULL) {
        char path[ZCS_MP4_PATH_MAX];
        struct stat st;
        size_t n;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        n = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            rec_collect_files(path, arr, cap, count);
            continue;
        }
        if (!S_ISREG(st.st_mode) || strstr(ent->d_name, ".mp4") == NULL) {
            continue;
        }
        if (*count >= *cap) {
            size_t nc = *cap ? *cap * 2 : 64;
            rec_file_ent *na = (rec_file_ent *)realloc(*arr, nc * sizeof(**arr));
            if (!na) {
                continue;
            }
            *arr = na;
            *cap = nc;
        }
        snprintf((*arr)[*count].path, sizeof((*arr)[*count].path), "%s", path);
        (*arr)[*count].mtime = st.st_mtime;
        (*arr)[*count].size = (uint64_t)st.st_size;
        ++*count;
    }
    closedir(dp);
}

static int rec_ent_cmp(const void *a, const void *b)
{
    const rec_file_ent *x = (const rec_file_ent *)a;
    const rec_file_ent *y = (const rec_file_ent *)b;

    if (x->mtime < y->mtime) {
        return -1;
    }
    if (x->mtime > y->mtime) {
        return 1;
    }
    return strcmp(x->path, y->path);
}

/* 总量超限时从最旧文件删起，直到回落到上限内。返回删除数。 */
/* 先收集全部 mp4（递归子目录）→ 累加总字节 → 按 mtime 升序排序 → 从头删。
 * 至少保留 1 个文件（count<=1 不删），避免正在录制的那个被误删。 */
static int rec_enforce_total_limit(void)
{
    rec_file_ent *arr = NULL;
    size_t cap = 0;
    size_t count = 0;
    uint64_t total = 0;
    size_t i;

    if (g_record_max_total_bytes == 0) {
        return 0;
    }
    rec_collect_files(zcs_mp4_recorder_root(), &arr, &cap, &count);
    for (i = 0; i < count; ++i) {
        total += arr[i].size;
    }
    if (total <= g_record_max_total_bytes || count <= 1) {
        free(arr);
        return 0;
    }
    qsort(arr, count, sizeof(*arr), rec_ent_cmp);
    {
        int removed = 0;

        for (i = 0; i < count && total > g_record_max_total_bytes; ++i) {
            if (remove(arr[i].path) == 0) {
                total -= arr[i].size;
                ++removed;
                zn_info("MP4 record retention: removed %s (%llu bytes)", arr[i].path,
                         (unsigned long long)arr[i].size);
            }
        }
        free(arr);
        return removed;
    }
}

/* 录制前磁盘剩余检查；返回 0 表示可继续录制。 */
static int rec_disk_has_space(void)
{
    struct statvfs vfs;

    if (g_record_free_space_bytes == 0) {
        return 1;
    }
    if (statvfs(zcs_mp4_recorder_root(), &vfs) != 0) {
        return 1; /* 统计失败不阻断录制 */
    }
    return (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize >= g_record_free_space_bytes;
}
#else
static int rec_enforce_total_limit(void) { return 0; }
static int rec_disk_has_space(void) { return 1; }
#endif

/* Absolute paths (ZLM accepts them) are compared against resolved roots. */
static int path_abs_under(const char *path, const char *root)
{
#if defined(_WIN32)
    char *ap = _fullpath(NULL, path, 0);
    char *ar = _fullpath(NULL, root, 0);
    int ok = 0;

    if (ap && ar) {
        size_t rl;
        path_norm_slashes(ap);
        path_norm_slashes(ar);
        rl = strlen(ar);
        while (rl > 0 && ar[rl - 1] == '/') {
            ar[--rl] = '\0';
        }
        if (rl > 0 && _strnicmp(ap, ar, rl) == 0 && (ap[rl] == '\0' || ap[rl] == '/')) {
            ok = 1;
        }
    }
    free(ap);
    free(ar);
    return ok;
#else
    char ap[4096];
    char ar[4096];
    size_t rl;

    if (!realpath(path, ap) || !realpath(root, ar)) {
        return 0;
    }
    rl = strlen(ar);
    while (rl > 0 && ar[rl - 1] == '/') {
        ar[--rl] = '\0';
    }
    return rl > 0 && strncmp(ap, ar, rl) == 0 && (ap[rl] == '\0' || ap[rl] == '/');
#endif
}

/*
 * 校验路径是否落在录像/点播允许的根目录下（规范化 + 前缀匹配）。
 * 兼容历史默认目录 ./www/vod 与 ./www/record，并处理绝对路径（realpath/
 * _fullpath 解析后比较）。这是比 ".." 更严格的第二道防线：即使没有 dotdot，
 * 也要求最终路径确实在受信根之内。
 */
int zcs_mp4_recorder_path_under_root(const char *file_path)
{
    char root[ZCS_CFG_PATH_MAX];
    char www_root[ZCS_CFG_PATH_MAX];
    char path[ZCS_MP4_PATH_MAX];
    size_t root_len;

    if (!file_path || !file_path[0] || path_has_dotdot(file_path)) {
        return 0;
    }
    strncpy(path, file_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    path_norm_slashes(path);

    strncpy(root, zcs_mp4_recorder_root(), sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    path_norm_slashes(root);
    while (root[0] && (root[strlen(root) - 1] == '/' || root[strlen(root) - 1] == '\\')) {
        root[strlen(root) - 1] = '\0';
    }
    root_len = strlen(root);
    if (root_len > 0 && strncmp(path, root, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/')) {
        return 1;
    }

    /* 兼容历史点播：./www/vod/...；录像：./www/record/... */
    strncpy(www_root, "./www", sizeof(www_root) - 1);
    www_root[sizeof(www_root) - 1] = '\0';
    root_len = strlen(www_root);
    if (strncmp(path, www_root, root_len) == 0 &&
        (path[root_len] == '\0' || path[root_len] == '/')) {
        return 1;
    }

    /* absolute path: resolve configured roots and compare */
    if (path[0] == '/' || (path[0] && path[1] == ':')) {
        if (path_abs_under(path, zcs_mp4_recorder_root())) {
            return 1;
        }
        if (path_abs_under(path, "./www")) {
            return 1;
        }
    }
    return 0;
}

static int mkdir_p(const char *dir)
{
    char tmp[ZCS_MP4_PATH_MAX];
    size_t i;
    size_t n;

    if (!dir || !dir[0]) {
        return -1;
    }
    n = strlen(dir);
    if (n >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, dir, n + 1);
    path_norm_slashes(tmp);
    for (i = 1; i < n; ++i) {
        if (tmp[i] != '/') {
            continue;
        }
        tmp[i] = '\0';
        if (tmp[0] && zcs_mp4_mkdir(tmp) != 0) {
#if defined(_WIN32)
            /* EEXIST ok */
#else
            /* ignore existing */
#endif
        }
        tmp[i] = '/';
    }
    if (zcs_mp4_mkdir(tmp) != 0) {
        /* may already exist */
    }
    return 0;
}

static int aac_is_adts(const uint8_t *es, size_t es_len)
{
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

static size_t aac_adts_hdr_len(const uint8_t *p, size_t left)
{
    if (!aac_is_adts(p, left)) {
        return 0;
    }
    return (p[1] & 0x01) ? 7u : 9u;
}

static int ensure_mux_buf(zcs_mp4_recorder *rec, size_t need)
{
    uint8_t *p;

    if (!rec) {
        return 0;
    }
    if (rec->mux_buf_cap >= need) {
        return 1;
    }
    need = need < 65536u ? 65536u : need;
    p = (uint8_t *)realloc(rec->mux_buf, need);
    if (!p) {
        return 0;
    }
    rec->mux_buf = p;
    rec->mux_buf_cap = need;
    return 1;
}

/* 后台关闭任务：持有已摘下的文件句柄与路径副本，不访问 recorder。 */
typedef struct zcs_mp4_close_job {
    mov_writer_t *mov;
    FILE *fp;
    char file_path[ZCS_MP4_PATH_MAX];
    char tmp_path[ZCS_MP4_PATH_MAX];
    char folder[ZCS_MP4_PATH_MAX];
    char file_name[256];
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    time_t start_unix;
    float time_len;
} zcs_mp4_close_job;

/* 在后台 IO 线程执行：关文件、改名、触发录制通知。 */
/*
 * 后台关闭任务（IO 线程执行）：关 mov/fp、按大小决定是否保留、临时名 rename
 * 为正式名、触发 webhook 通知，并顺带做一次容量治理。
 *
 * 关键防御：<1024 字节的文件视为“无有效画面”（只有容器头、没录到关键帧），
 * 直接删掉且不通知——否则会产生一堆 0 秒垃圾录像和误报。
 *
 * 之所以整个关闭流程放到 IO 线程：mov_writer_destroy/fflush/rename 都是磁盘
 * 操作，在 poller 上做会阻塞同线程的其它会话。
 */
static void mp4_close_job_work(void *user)
{
    zcs_mp4_close_job *j = (zcs_mp4_close_job *)user;
    struct stat st;
    int64_t file_size = 0;

    if (j->mov) {
        mov_writer_destroy(j->mov);
        j->mov = NULL;
    }
    if (j->fp) {
        fflush(j->fp);
        fclose(j->fp);
        j->fp = NULL;
    }
    if (j->tmp_path[0] && stat(j->tmp_path, &st) == 0) {
        file_size = (int64_t)st.st_size;
    }
    if (file_size < 1024) {
        /* 过小文件（无有效画面）直接删除，不触发通知。 */
        if (j->tmp_path[0]) {
            remove(j->tmp_path);
        }
        return;
    }
    if (j->tmp_path[0] && j->file_path[0]) {
        remove(j->file_path);
        if (rename(j->tmp_path, j->file_path) != 0) {
            zn_warn("MP4 record rename failed: %s -> %s", j->tmp_path, j->file_path);
        }
    }
    zcs_webhook_on_record_mp4(j->app, j->stream, j->file_name, j->file_path, j->folder, file_size,
                              (int64_t)j->start_unix, j->time_len);
    zn_info("MP4 record done: %s size=%lld time_len=%.2f", j->file_path, (long long)file_size,
             (double)j->time_len);
    /* 新切片落地后执行容量治理（在 IO 线程，遍历目录不阻塞 poller）。 */
    rec_enforce_total_limit();
}

static void mp4_close_job_on_io(void *user)
{
    free(user);
}

static void close_file_sync(zcs_mp4_recorder *rec);

/**
 * poller 侧：同步摘下 writer/句柄并重置状态，把耗时的关闭放到后台 IO 线程。
 * 这样切片和停推都不在 poller 上关文件。
 */
static void close_file_async(zcs_mp4_recorder *rec)
{
    zcs_mp4_close_job *j;

    if (!rec || !rec->mov) {
        return;
    }
    j = (zcs_mp4_close_job *)calloc(1, sizeof(*j));
    if (!j) {
        /* 内存不足：退回同步关闭，保证不丢文件。 */
        close_file_sync(rec);
        return;
    }
    j->mov = rec->mov;
    j->fp = rec->fp;
    j->start_unix = rec->start_unix;
    j->time_len = 0.f;
    if (rec->have_start_dts && rec->last_dts_ms >= rec->start_dts_ms) {
        j->time_len = (float)(rec->last_dts_ms - rec->start_dts_ms) / 1000.f;
    }
    snprintf(j->file_path, sizeof(j->file_path), "%s", rec->file_path);
    snprintf(j->tmp_path, sizeof(j->tmp_path), "%s", rec->tmp_path);
    snprintf(j->folder, sizeof(j->folder), "%s", rec->folder);
    snprintf(j->file_name, sizeof(j->file_name), "%s", rec->file_name);
    if (rec->src) {
        snprintf(j->app, sizeof(j->app), "%s", rec->src->app);
        snprintf(j->stream, sizeof(j->stream), "%s", rec->src->stream);
    }

    rec->mov = NULL;
    rec->fp = NULL;
    rec->file_path[0] = '\0';
    rec->tmp_path[0] = '\0';
    rec->folder[0] = '\0';
    rec->file_name[0] = '\0';
    rec->video_track = -1;
    rec->audio_track = -1;
    rec->video_armed = 0;
    rec->sent_video_cfg = 0;
    rec->sent_audio_cfg = 0;
    rec->have_start_dts = 0;
    zcs_mux_av_timeline_reset(&rec->mux_av);

    if (zcs_vod_thread_pool_run(NULL, mp4_close_job_work, mp4_close_job_on_io, j) != ZN_OK) {
        mp4_close_job_work(j);
        free(j);
    }
}

/* 同步关闭（仅在无法投递到线程池时使用）。 */
static void close_file_sync(zcs_mp4_recorder *rec)
{
    zcs_mp4_close_job j;

    if (!rec || !rec->mov) {
        return;
    }
    memset(&j, 0, sizeof(j));
    j.mov = rec->mov;
    j.fp = rec->fp;
    j.start_unix = rec->start_unix;
    if (rec->have_start_dts && rec->last_dts_ms >= rec->start_dts_ms) {
        j.time_len = (float)(rec->last_dts_ms - rec->start_dts_ms) / 1000.f;
    }
    snprintf(j.file_path, sizeof(j.file_path), "%s", rec->file_path);
    snprintf(j.tmp_path, sizeof(j.tmp_path), "%s", rec->tmp_path);
    snprintf(j.folder, sizeof(j.folder), "%s", rec->folder);
    snprintf(j.file_name, sizeof(j.file_name), "%s", rec->file_name);
    if (rec->src) {
        snprintf(j.app, sizeof(j.app), "%s", rec->src->app);
        snprintf(j.stream, sizeof(j.stream), "%s", rec->src->stream);
    }

    rec->mov = NULL;
    rec->fp = NULL;
    rec->file_path[0] = '\0';
    rec->tmp_path[0] = '\0';
    rec->folder[0] = '\0';
    rec->file_name[0] = '\0';
    rec->video_track = -1;
    rec->audio_track = -1;
    rec->video_armed = 0;
    rec->sent_video_cfg = 0;
    rec->sent_audio_cfg = 0;
    rec->have_start_dts = 0;
    zcs_mux_av_timeline_reset(&rec->mux_av);

    mp4_close_job_work(&j);
}

static void path_sanitize_stream(char *stream, size_t cap)
{
    size_t i;
    if (!stream || cap == 0) {
        return;
    }
    for (i = 0; stream[i] && i + 1 < cap; ++i) {
        char c = stream[i];
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') {
            stream[i] = '_';
        }
    }
    if (strstr(stream, "..") != NULL) {
        strncpy(stream, "stream", cap - 1);
        stream[cap - 1] = '\0';
    }
}

static int open_new_file(zcs_mp4_recorder *rec)
{
    time_t now;
    struct tm tm_now;
    char date[32];
    char stamp[32];
    char root[ZCS_CFG_PATH_MAX];
    char stream[ZCS_STREAM_MAX];
    int n;
    int seq;

    if (!rec || !rec->src) {
        return -1;
    }
    if (!rec_disk_has_space()) {
        zn_warn("MP4 record paused: free space under threshold (%d MB)",
                 (int)(g_record_free_space_bytes / (1024u * 1024u)));
        return -1;
    }
    strncpy(stream, rec->src->stream[0] ? rec->src->stream : "stream", sizeof(stream) - 1);
    stream[sizeof(stream) - 1] = '\0';
    path_sanitize_stream(stream, sizeof(stream));

    now = time(NULL);
    rec->start_unix = now;
#if defined(_WIN32)
    localtime_s(&tm_now, &now);
#else
    localtime_r(&now, &tm_now);
#endif
    strftime(date, sizeof(date), "%Y-%m-%d", &tm_now);
    strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm_now);

    strncpy(root, zcs_mp4_recorder_root(), sizeof(root) - 1);
    root[sizeof(root) - 1] = '\0';
    path_norm_slashes(root);
    while (root[0] && root[strlen(root) - 1] == '/') {
        root[strlen(root) - 1] = '\0';
    }

    /* {root}/{stream}/{YYYY-MM-dd}/ */
    n = snprintf(rec->folder, sizeof(rec->folder), "%s/%s/%s", root, stream, date);
    if (n < 0 || (size_t)n >= sizeof(rec->folder)) {
        return -1;
    }
    if (mkdir_p(rec->folder) != 0) {
        zn_warn("MP4 record mkdir failed: %s", rec->folder);
    }

    /* {YYYYMMDDHHMMSS}.mp4；同秒冲突则 _1 _2 … */
    /* 用 access 探测重名：同一秒内可能因快速滚动切出多个文件，直接覆盖会
     * 丢录像，加后缀保证唯一。100 次仍冲突说明命名撞得异常，放弃本次。 */
    for (seq = 0; seq < 100; ++seq) {
        if (seq == 0) {
            n = snprintf(rec->file_name, sizeof(rec->file_name), "%s.mp4", stamp);
        } else {
            n = snprintf(rec->file_name, sizeof(rec->file_name), "%s_%d.mp4", stamp, seq);
        }
        if (n < 0 || (size_t)n >= sizeof(rec->file_name)) {
            return -1;
        }
        n = snprintf(rec->file_path, sizeof(rec->file_path), "%s/%s", rec->folder, rec->file_name);
        if (n < 0 || (size_t)n >= sizeof(rec->file_path)) {
            return -1;
        }
        if (zcs_mp4_access(rec->file_path) != 0) {
            break;
        }
    }
    if (seq >= 100) {
        zn_warn("MP4 record name exhausted under %s", rec->folder);
        return -1;
    }

    /* 先写临时名，关闭后 rename 为正式名，避免半成品被访问。 */
    n = snprintf(rec->tmp_path, sizeof(rec->tmp_path), "%s/.%s", rec->folder, rec->file_name);
    if (n < 0 || (size_t)n >= sizeof(rec->tmp_path)) {
        return -1;
    }

    rec->fp = fopen(rec->tmp_path, "wb+");
    if (!rec->fp) {
        zn_warn("MP4 record open failed: %s", rec->tmp_path);
        rec->file_path[0] = '\0';
        rec->tmp_path[0] = '\0';
        return -1;
    }
    /* MOV_FLAG_FASTSTART：把 moov(索引) 前移到文件头附近，播放器无需读完整
     * 文件就能起播/seek；代价是关闭时要回填重排（放到后台 IO 做）。
     * zcs_mov_file_buffer 提供带 seek 的文件写缓冲，FASTSTART 需要随机写。 */
    rec->mov = mov_writer_create(zcs_mov_file_buffer(), rec->fp, MOV_FLAG_FASTSTART);
    if (!rec->mov) {
        fclose(rec->fp);
        rec->fp = NULL;
        rec->file_path[0] = '\0';
        rec->tmp_path[0] = '\0';
        return -1;
    }
    rec->video_track = -1;
    rec->audio_track = -1;
    rec->video_armed = 0;
    rec->sent_video_cfg = 0;
    rec->sent_audio_cfg = 0;
    rec->have_start_dts = 0;
    zcs_mux_av_timeline_reset(&rec->mux_av);
    zn_info("MP4 record start: %s", rec->file_path);
    return 0;
}

/*
 * 懒创建视频/音频 track。必须在拿到源 config（SPS/PPS、ASC）之后才能建轨，
 * 所以配置未到时返回 0 让 tick 下轮再试；视频轨建不出来（编码不支持）才
 * 返回 -1。宽高缺省 1920x1080 只是兜底，真实值应来自源元数据。
 * 视频轨先建、音频后建：mov 的轨道顺序影响部分播放器的默认轨道选择。
 */
static int ensure_tracks(zcs_mp4_recorder *rec)
{
    const uint8_t *cfg = NULL;
    size_t clen = 0;
    const uint8_t *extra = NULL;
    size_t extra_len = 0;
    int w = 0;
    int h = 0;
    zcs_codec_id vcodec;

    if (!rec || !rec->mov || !rec->src || !rec->src->gop_queue) {
        return -1;
    }

    if (!rec->sent_video_cfg) {
        cfg = zcs_gop_queue_video_config(rec->src->gop_queue, &clen);
        if (!cfg || clen < 2) {
            return 0; /* wait */
        }
        vcodec = zcs_flv_video_config_codec(cfg, clen);
        if (vcodec == ZCS_CODEC_INVALID) {
            vcodec = rec->src->video.codec;
        }
        rec->video_codec = vcodec;
        w = (int)rec->src->video.width;
        h = (int)rec->src->video.height;
        if (w <= 0) {
            w = 1920;
        }
        if (h <= 0) {
            h = 1080;
        }

        if (vcodec == ZCS_CODEC_H264 && zcs_rtmp_avc_extradata(cfg, clen, &extra, &extra_len) &&
            extra && extra_len > 0) {
            (void)zcs_avc_config_load_record(&rec->avc, extra, extra_len);
            rec->video_track =
                mov_writer_add_video(rec->mov, MOV_OBJECT_H264, w, h, extra, extra_len);
        } else if (vcodec == ZCS_CODEC_H265 &&
                   zcs_h265_video_config_hvcc(cfg, clen, &extra, &extra_len) && extra &&
                   extra_len > 0) {
            (void)zcs_hevc_config_load_record(&rec->hevc, extra, extra_len);
            rec->video_track =
                mov_writer_add_video(rec->mov, MOV_OBJECT_H265, w, h, extra, extra_len);
        } else {
            zn_warn("MP4 record: unsupported video cfg codec=%d", (int)vcodec);
            return -1;
        }
        if (rec->video_track < 0) {
            return -1;
        }
        rec->sent_video_cfg = 1;
    }

    if (!rec->sent_audio_cfg && rec->src->has_audio) {
        const uint8_t *acfg = NULL;
        size_t alen = 0;
        const uint8_t *asc = NULL;
        size_t asc_len = 0;
        int sr = 0;
        int ch = 0;

        acfg = zcs_gop_queue_audio_config(rec->src->gop_queue, &alen);
        if (acfg && alen > 0 && zcs_rtmp_aac_extradata(acfg, alen, &asc, &asc_len) && asc &&
            asc_len > 0) {
            zcs_aac_config_set_defaults(&rec->aac, 44100, 2);
            if (zcs_aac_config_load_asc(&rec->aac, asc, asc_len)) {
                sr = zcs_aac_config_sample_rate(&rec->aac);
                ch = zcs_aac_config_channels(&rec->aac);
                if (sr <= 0) {
                    sr = (int)rec->src->audio.sample_rate;
                }
                if (ch <= 0) {
                    ch = (int)rec->src->audio.channels;
                }
                if (sr <= 0) {
                    sr = 48000;
                }
                if (ch <= 0) {
                    ch = 2;
                }
                rec->audio_track =
                    mov_writer_add_audio(rec->mov, MOV_OBJECT_AAC, ch, 16, sr, asc, asc_len);
                if (rec->audio_track >= 0) {
                    rec->have_aac = 1;
                    rec->sent_audio_cfg = 1;
                }
            }
        }
    }
    return rec->sent_video_cfg ? 1 : 0;
}

/* 录制 H.264。首个 sync（IDR/SPS 起始）才“上膛”并锁定时间轴原点，之前的
 * 帧丢弃（录像文件必须从可解码点开始）。keyframe 标志按是否 IDR 写
 * MOV_AV_FLAG_KEYFREAME，供播放器 seek。vcl/update 由转换函数回写，用于
 * 判断是否真的有画面数据。 */
static void feed_h264(zcs_mp4_recorder *rec, const uint8_t *annexb, size_t len, uint32_t dts_ms,
                      int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts;
    int64_t rel_pts;
    int flags;
    int sync;
    int idr;
    uint32_t pts_ms = dts_ms;

    if (!rec || rec->video_track < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zcs_h264_annexb_is_sync_key(annexb, len);
    idr = keyframe || zcs_h264_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        rec->video_armed = 1;
        zcs_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->start_dts_ms = dts_ms;
        rec->have_start_dts = 1;
    }
    if (!ensure_mux_buf(rec, len + 65536u)) {
        return;
    }
    n = zcs_avc_config_annexb_to_mp4(&rec->avc, annexb, len, rec->mux_buf, rec->mux_buf_cap, &vcl,
                                     &update);
    if (n <= 0) {
        return;
    }
    rel_dts = (int64_t)zcs_mux_av_timeline_pts(&rec->mux_av, ZCS_TRACK_VIDEO, dts_ms);
    rel_pts = (int64_t)zcs_mux_av_timeline_pts(&rec->mux_av, ZCS_TRACK_VIDEO, pts_ms);
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)mov_writer_write(rec->mov, rec->video_track, rec->mux_buf, (size_t)n, rel_pts, rel_dts,
                           flags);
    rec->last_dts_ms = dts_ms;
}

/* H.265 录制，逻辑同 feed_h264。注意这里用 rel_dts 同时当 pts（H.265 路径
 * 未单独跟踪 pts），与 H.264 版本的差别仅此一处。 */
static void feed_h265(zcs_mp4_recorder *rec, const uint8_t *annexb, size_t len, uint32_t dts_ms,
                      int keyframe)
{
    int vcl = 0;
    int update = 0;
    int n;
    int64_t rel_dts;
    int flags;
    int sync;
    int idr;

    if (!rec || rec->video_track < 0 || !annexb || len < 4) {
        return;
    }
    sync = keyframe || zcs_h265_annexb_is_sync_key(annexb, len);
    idr = keyframe || zcs_h265_annexb_is_idr(annexb, len);
    if (!rec->video_armed) {
        if (!sync) {
            return;
        }
        rec->video_armed = 1;
        zcs_mux_av_timeline_lock_origin(&rec->mux_av, dts_ms);
        rec->start_dts_ms = dts_ms;
        rec->have_start_dts = 1;
    }
    if (!ensure_mux_buf(rec, len + 65536u)) {
        return;
    }
    n = zcs_hevc_config_annexb_to_mp4(&rec->hevc, annexb, len, rec->mux_buf, rec->mux_buf_cap, &vcl,
                                      &update);
    if (n <= 0) {
        return;
    }
    rel_dts = (int64_t)zcs_mux_av_timeline_pts(&rec->mux_av, ZCS_TRACK_VIDEO, dts_ms);
    flags = idr ? MOV_AV_FLAG_KEYFREAME : 0;
    (void)mov_writer_write(rec->mov, rec->video_track, rec->mux_buf, (size_t)n, rel_dts, rel_dts,
                           flags);
    rec->last_dts_ms = dts_ms;
}

/* 录制 AAC。视频未上膛前不写音频（保证文件以视频关键帧起始，避免 a/v 起点
 * 错位）。内部存储是 raw AAC，若输入仍带 ADTS 头（ffmpeg 常见），按头长把
 * 其剥掉再写。 */
static void feed_aac(zcs_mp4_recorder *rec, const uint8_t *es, size_t len, uint32_t dts_ms)
{
    const uint8_t *raw = es;
    size_t raw_len = len;
    size_t hdr;
    int64_t rel_dts;

    if (!rec || rec->audio_track < 0 || !es || len == 0 || !rec->video_armed) {
        return;
    }
    if (aac_is_adts(es, len)) {
        hdr = aac_adts_hdr_len(es, len);
        if (hdr == 0 || len <= hdr) {
            return;
        }
        raw = es + hdr;
        raw_len = len - hdr;
    }
    rel_dts = (int64_t)zcs_mux_av_timeline_pts(&rec->mux_av, ZCS_TRACK_AUDIO, dts_ms);
    (void)mov_writer_write(rec->mov, rec->audio_track, raw, raw_len, rel_dts, rel_dts, 0);
    if (dts_ms > rec->last_dts_ms) {
        rec->last_dts_ms = dts_ms;
    }
}

/* 只在关键帧处切文件：每个 mp4 片段都要能以 IDR 起播，非关键帧切片会得到
 * 无法独立解码的坏片段。时长按 (last_dts - start_dts) 与 max_second 比较；
 * 未上膛或时间回退时不切。 */
static int should_rotate(zcs_mp4_recorder *rec, int keyframe)
{
    uint32_t dur_ms;

    if (!rec || !rec->have_start_dts || !keyframe || !rec->video_armed) {
        return 0;
    }
    if (rec->last_dts_ms < rec->start_dts_ms) {
        return 0;
    }
    dur_ms = rec->last_dts_ms - rec->start_dts_ms;
    return (int)dur_ms >= rec->max_second * 1000;
}

/*
 * 单次 tick（调用方持 mu）。状态机式推进：
 *   无文件→开新文件；track 未建→ensure_tracks（等配置）；然后本轮读最多
 *   ZCS_MP4_REC_TICK_FRAMES 帧。视频帧遇到该切片的时机（时长到且是关键帧）
 *   就异步关旧文件并开新文件；音频仅在视频已上膛后写入。
 * 任何一步失败（如磁盘满、开文件失败）直接 return，下个 tick 再试，不在这里
 * 重试纠缠。
 */
static void tick_nolock(zcs_mp4_recorder *rec)
{
    zcs_gop_slot slot;
    int n;

    if (!rec || rec->closing || !rec->reader) {
        return;
    }
    if (!rec->mov) {
        if (open_new_file(rec) != 0) {
            return;
        }
    }
    if (ensure_tracks(rec) <= 0) {
        return;
    }

    for (n = 0; n < ZCS_MP4_REC_TICK_FRAMES && zcs_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
         ++n) {
        if (slot.track == ZCS_TRACK_VIDEO) {
            int key = slot.keyframe;
            if (should_rotate(rec, key)) {
                /* 异步关闭旧文件（不让 poller 等磁盘 IO），立即开新文件。
                 * 新文件的 track/时刻状态在 open_new_file + ensure_tracks 里
                 * 重置，本关键帧随即成为新文件的起点。 */
                close_file_async(rec);
                if (open_new_file(rec) != 0 || ensure_tracks(rec) <= 0) {
                    return;
                }
                /* 切文件后从本关键帧重新武装 */
            }
            if (rec->video_codec == ZCS_CODEC_H265 || slot.codec == ZCS_CODEC_H265) {
                feed_h265(rec, slot.data, slot.len, slot.dts_ms, key);
            } else {
                feed_h264(rec, slot.data, slot.len, slot.dts_ms, key);
            }
        } else if (slot.track == ZCS_TRACK_AUDIO) {
            if (slot.codec == ZCS_CODEC_AAC || rec->have_aac) {
                feed_aac(rec, slot.data, slot.len, slot.dts_ms);
            }
        }
    }
}

static void tick_cb(void *user)
{
    zcs_mp4_recorder *rec = (zcs_mp4_recorder *)user;

    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (!rec->closing) {
        tick_nolock(rec);
    }
    zb_mutex_unlock(rec->mu);
}

/* 销毁：停定时器后，在持锁状态下置 closing、摘 reader、解绑源，再在锁外
 * 销毁互斥量与内存。close_file_async 会异步落盘并触发 capacity 治理。
 * src->mp4_recorder 只有在仍指向自己时才清，避免误清新接入的录制器。 */
static void destroy_recorder(zcs_mp4_recorder *rec)
{
    zcs_media_source *src;

    if (!rec) {
        return;
    }
    if (rec->timer) {
        zn_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    src = rec->src;
    if (rec->mu) {
        zb_mutex_lock(rec->mu);
        rec->closing = 1;
        close_file_async(rec);
        if (rec->reader) {
            zcs_gop_reader_detach(rec->reader);
            rec->reader = NULL;
        }
        if (src && src->mp4_recorder == rec) {
            src->mp4_recorder = NULL;
            src->mp4_recorder_stop = NULL;
        }
        zb_mutex_unlock(rec->mu);
        zb_mutex_destroy(rec->mu);
        rec->mu = NULL;
    } else {
        close_file_async(rec);
        if (rec->reader) {
            zcs_gop_reader_detach(rec->reader);
        }
        if (src && src->mp4_recorder == rec) {
            src->mp4_recorder = NULL;
            src->mp4_recorder_stop = NULL;
        }
    }
    free(rec->mux_buf);
    free(rec);
}

zn_err_t zcs_mp4_recorder_start(zcs_media_source *src, zn_poller *poller)
{
    zcs_mp4_recorder *rec;
    zn_poller *pol;

    if (!src || !src->gop_queue) {
        return ZN_ERR_INVALID;
    }
    if (!src->enable_mp4) {
        return ZN_OK;
    }
    if (src->mp4_recorder) {
        return ZN_OK;
    }
    if (!g_record_root[0]) {
        strncpy(g_record_root, "./www/record", sizeof(g_record_root) - 1);
    }

    rec = (zcs_mp4_recorder *)calloc(1, sizeof(*rec));
    if (!rec) {
        return ZN_ERR_NOMEM;
    }
    rec->src = src;
    rec->video_track = -1;
    rec->audio_track = -1;
    rec->max_second = g_mp4_max_second > 0 ? g_mp4_max_second : 180;
    rec->mu = zb_mutex_create(0);
    if (!rec->mu) {
        free(rec);
        return ZN_ERR_NOMEM;
    }
    rec->mux_buf_cap = 256 * 1024;
    rec->mux_buf = (uint8_t *)malloc(rec->mux_buf_cap);
    if (!rec->mux_buf) {
        zb_mutex_destroy(rec->mu);
        free(rec);
        return ZN_ERR_NOMEM;
    }
    zcs_mux_av_timeline_reset(&rec->mux_av);

    rec->reader = zcs_gop_reader_attach(src->gop_queue);
    if (!rec->reader) {
        free(rec->mux_buf);
        zb_mutex_destroy(rec->mu);
        free(rec);
        return ZN_ERR_NOMEM;
    }
    /* 从关键帧起读：录像文件必须以可解码帧开头，故 reader 直接 seek 到 live
     * key。poller 未显式给出时回落到全局媒体事件 poller，保证定时器总有着落。 */
    zcs_gop_reader_seek_live_key(rec->reader);

    src->mp4_recorder = rec;
    src->mp4_recorder_stop = zcs_mp4_recorder_stop;
    pol = poller ? poller : zcs_media_events_poller();
    if (pol) {
        rec->timer_poller = pol;
        rec->timer = zn_timer_start(pol, ZCS_MP4_REC_TICK_MS, 1, tick_cb, rec);
    }
    zn_info("MP4 recorder attached: %s/%s max_sec=%d", src->app, src->stream, rec->max_second);
    return ZN_OK;
}

/* 在 rec->timer_poller 上真正销毁。 */
static void stop_on_owner(void *user)
{
    destroy_recorder((zcs_mp4_recorder *)user);
}

/* 停止录制器。停止可能从任意线程/poller 触发（如平台控制、断流、配置变更），
 * 而 timer/reader 属于启动它的 timer_poller，跨 poller 直接 destroy 会有竞态，
 * 故投递回 owner poller 执行；只有已在 owner 线程或投递失败时才就地 destroy。
 * stop_claimed 用 CAS 保证只销毁一次（多个来源可能同时调 stop）。 */
void zcs_mp4_recorder_stop(zcs_media_source *src)
{
    zcs_mp4_recorder *rec;

    if (!src || !src->mp4_recorder) {
        return;
    }
    rec = (zcs_mp4_recorder *)src->mp4_recorder;
    if (!zb_atomic_cas32(&rec->stop_claimed, 0, 1)) {
        return; /* 已有人认领停止 */
    }
    /* 录制器的 timer、reader 与内部状态属于启动它的 poller：
     * 从别的 poller 触发停止必须投递回去，不能在这里直接 destroy。 */
    if (rec->timer_poller && !zn_poller_is_current_thread(rec->timer_poller)) {
        if (zn_poller_async(rec->timer_poller, stop_on_owner, rec, 0) == ZN_OK) {
            return;
        }
    }
    destroy_recorder(rec);
}
