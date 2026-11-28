/**
 * @file record_index.c
 * @brief 通用录像索引：按通道/时间范围扫描 {root}/{stream}/{date}/{HHMMSS}.mp4。
 */
#include "zcs/device/core/device_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#if defined(_MSC_VER)
#define strcasecmp _stricmp
#endif
#else
#include <dirent.h>
#endif

static char g_record_root[512];

void zcs_device_core_set_record_root(const char *root)
{
    snprintf(g_record_root, sizeof(g_record_root), "%s", root ? root : "");
}

const char *zcs_device_core_record_root(void)
{
    return g_record_root;
}

static void format_iso(time_t t, char *out, size_t cap)
{
    struct tm tmv;

#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%Y-%m-%dT%H:%M:%S", &tmv);
}

static int parse_time_str(const char *s, time_t *out)
{
    int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
    struct tm tmv;

    if (!s || !s[0]) {
        return -1;
    }
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6 &&
        sscanf(s, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) {
        return -1;
    }
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900;
    tmv.tm_mon = M - 1;
    tmv.tm_mday = D;
    tmv.tm_hour = h;
    tmv.tm_min = m;
    tmv.tm_sec = sec;
    tmv.tm_isdst = -1;
    *out = mktime(&tmv);
    return *out == (time_t)-1 ? -1 : 0;
}

/**
 * 私有定义：对外是不透明句柄（见 device_core.h）。
 * 128 条 × 784B ≈ 98KB，只在查询时临时分配，不进调用方栈。
 */
struct zcs_device_record_list {
    zcs_device_record_entry items[ZCS_DEVICE_RECORD_MAX];
    size_t count;
};

typedef struct record_scan_ctx {
    zcs_device_record_list *list;
    const char *channel_id;
    const char *stream;
    time_t q_start;
    time_t q_end;
    int has_q;
} record_scan_ctx;

static void add_record(record_scan_ctx *ctx, const char *date, const char *fname, time_t mtime)
{
    zcs_device_record_entry *e;
    char ts[32];
    int Y = 0, M = 0, D = 0, h = 0, m = 0, s = 0;
    time_t start = mtime;
    time_t end = mtime;

    if (ctx->list->count >= ZCS_DEVICE_RECORD_MAX) {
        return;
    }
    /* 文件名即起录时刻（YYYYMMDDHHMMSS），比 mtime 更可靠 */
    if (sscanf(fname, "%4d%2d%2d%2d%2d%2d", &Y, &M, &D, &h, &m, &s) == 6) {
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year = Y - 1900;
        tmv.tm_mon = M - 1;
        tmv.tm_mday = D;
        tmv.tm_hour = h;
        tmv.tm_min = m;
        tmv.tm_sec = s;
        tmv.tm_isdst = -1;
        start = mktime(&tmv);
    }
    if (start == (time_t)-1) {
        start = mtime;
    }
    if (end < start) {
        end = start + 60;
    }
    if (ctx->has_q && end < ctx->q_start) {
        return;
    }
    if (ctx->has_q && start > ctx->q_end) {
        return;
    }
    e = &ctx->list->items[ctx->list->count];
    memset(e, 0, sizeof(*e));
    snprintf(e->channel_id, sizeof(e->channel_id), "%s", ctx->channel_id ? ctx->channel_id : "");
    snprintf(e->name, sizeof(e->name), "%s", fname);
    snprintf(e->file_path, sizeof(e->file_path), "%s/%s/%s", ctx->stream, date, fname);
    format_iso(start, ts, sizeof(ts));
    snprintf(e->start_time, sizeof(e->start_time), "%s", ts);
    format_iso(end, ts, sizeof(ts));
    snprintf(e->end_time, sizeof(e->end_time), "%s", ts);
    snprintf(e->type, sizeof(e->type), "%s", "time");
    ctx->list->count++;
}

#if defined(_WIN32)
static void win_path(const char *in, char *out, size_t cap)
{
    size_t i = 0;

    for (; in && *in && i + 1 < cap; ++in) {
        out[i++] = (*in == '/') ? '\\' : *in;
    }
    out[i] = '\0';
}
#endif

static int ends_with_mp4(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && strcasecmp(name + n - 4, ".mp4") == 0;
}

#if defined(_WIN32)
static void scan_files_win(record_scan_ctx *ctx, const char *root_stream, const char *date)
{
    char pattern[768];
    char base[768];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    win_path(root_stream, base, sizeof(base));
    snprintf(pattern, sizeof(pattern), "%s\\%s\\*.mp4", base, date);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        char full[768];
        struct _stat64 st;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !ends_with_mp4(fd.cFileName)) {
            continue;
        }
        snprintf(full, sizeof(full), "%s\\%s\\%s", base, date, fd.cFileName);
        if (_stat64(full, &st) != 0) {
            continue;
        }
        add_record(ctx, date, fd.cFileName, (time_t)st.st_mtime);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void scan_dates_win(record_scan_ctx *ctx, const char *root_stream)
{
    char pattern[768];
    char base[768];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    win_path(root_stream, base, sizeof(base));
    snprintf(pattern, sizeof(pattern), "%s\\*", base);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') {
            continue;
        }
        scan_files_win(ctx, base, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void scan_files_posix(record_scan_ctx *ctx, const char *root_stream, const char *date)
{
    char dir[768];
    DIR *d;
    struct dirent *de;

    snprintf(dir, sizeof(dir), "%s/%s", root_stream, date);
    d = opendir(dir);
    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        char full[768];
        struct stat st;
        if (!ends_with_mp4(de->d_name)) {
            continue;
        }
        snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
        if (stat(full, &st) != 0) {
            continue;
        }
        add_record(ctx, date, de->d_name, st.st_mtime);
    }
    closedir(d);
}

static void scan_dates_posix(record_scan_ctx *ctx, const char *root_stream)
{
    DIR *d = opendir(root_stream);
    struct dirent *de;

    if (!d) {
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        scan_files_posix(ctx, root_stream, de->d_name);
    }
    closedir(d);
}
#endif

static int collect_records(const char *channel_id, const char *start_time, const char *end_time,
                           zcs_device_record_list *out)
{
    zcs_device_channel_info ch;
    char stream[ZCS_DEVICE_STREAM_MAX];
    char root_stream[768];
    const char *root = g_record_root;
    record_scan_ctx ctx;
    time_t qs = 0;
    time_t qe = 0;

    if (!out || !channel_id) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (zcs_device_core_get(channel_id, &ch) != 0 || !root[0]) {
        return -1;
    }
    snprintf(stream, sizeof(stream), "%s", ch.stream);
    snprintf(root_stream, sizeof(root_stream), "%s/%s", root, stream);
    memset(&ctx, 0, sizeof(ctx));
    ctx.list = out;
    ctx.channel_id = channel_id;
    ctx.stream = stream;
    if (parse_time_str(start_time, &qs) == 0 && parse_time_str(end_time, &qe) == 0 && qe >= qs) {
        ctx.q_start = qs;
        ctx.q_end = qe;
        ctx.has_q = 1;
    }
#if defined(_WIN32)
    scan_dates_win(&ctx, root_stream);
#else
    scan_dates_posix(&ctx, root_stream);
#endif
    /* 按开始时间升序（插入排序，量小） */
    if (out->count > 1) {
        size_t i;
        for (i = 1; i < out->count; ++i) {
            zcs_device_record_entry tmp = out->items[i];
            size_t j = i;
            while (j > 0 && strcmp(out->items[j - 1].start_time, tmp.start_time) > 0) {
                out->items[j] = out->items[j - 1];
                --j;
            }
            out->items[j] = tmp;
        }
    }
    return 0;
}

zcs_device_record_list *zcs_device_record_query(const char *channel_id, const char *start_time,
                                                const char *end_time)
{
    zcs_device_record_list *out = (zcs_device_record_list *)calloc(1, sizeof(*out));

    if (!out) {
        return NULL;
    }
    if (collect_records(channel_id, start_time, end_time, out) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

void zcs_device_record_list_free(zcs_device_record_list *list)
{
    free(list);
}

size_t zcs_device_record_list_count(const zcs_device_record_list *list)
{
    return list ? list->count : 0;
}

const zcs_device_record_entry *zcs_device_record_list_at(const zcs_device_record_list *list,
                                                         size_t index)
{
    if (!list || index >= list->count) {
        return NULL;
    }
    return &list->items[index];
}

int zcs_device_record_pick(const char *channel_id, const char *start_time, const char *end_time,
                           zcs_device_record_entry *out, int64_t *offset_ms)
{
    zcs_device_record_list *list;
    time_t qs = 0;
    time_t qe = 0;
    int rc = -1;
    int have_q;

    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    if (offset_ms) {
        *offset_ms = 0;
    }
    list = (zcs_device_record_list *)calloc(1, sizeof(*list));
    if (!list) {
        return -1;
    }
    if (collect_records(channel_id, start_time, end_time, list) != 0 || list->count == 0) {
        free(list);
        return -1;
    }
    have_q = parse_time_str(start_time, &qs) == 0 && parse_time_str(end_time, &qe) == 0;
    {
        const zcs_device_record_entry *e = &list->items[0];
        time_t rs = 0;

        *out = *e;
        rc = 0;
        if (offset_ms && have_q && parse_time_str(e->start_time, &rs) == 0 && qs > rs) {
            *offset_ms = (int64_t)(qs - rs) * 1000;
        }
    }
    free(list);
    return rc;
}
