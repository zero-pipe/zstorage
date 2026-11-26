/**
 * @file response.c
 * @brief HTTP 响应发送（错误/JSON/字节/静态文件/VOD 文件）。
 *
 * 两条硬规则（都踩过坑，破坏任意一条都会表现为“客户端一直卡住”）：
 *
 * 1. **响应头必须先完整构造成功再发送。** 头超长/格式化失败时不能发送半截
 *    响应，只能直接断开连接。历史上曾把 vsnprintf 的返回值当长度发送，
 *    导致头被截断、CORS 字段丢失，HLS 播放端就此挂起等待。
 *    见 http_build_header 及各处 `if (n < 0)` 分支。
 *
 * 2. **文件 I/O 不得在 poller 线程上同步做。** open/close/fread/stat 全部
 *    投递到 VOD 后台线程池，poller 只负责发数据。否则一个大文件读取就能
 *    把整个 poller 上的所有会话冻住。
 *    见 zms_http_close_file_async / file_read_submit 及其 *_on_io 回调。
 *
 * 异步任务的会话生命周期：投递前调 zms_http_session_async_hold，回调里
 * 无条件 async_release。会话可能在异步期间被关闭，此时 hs->closed 已置位，
 * release 会在最后一个引用离开时真正释放。
 */
#include "zms/play/vod_play_port.h"
#include "session/http/session_internal.h"
#include "zn/platform.h"
#include "zms/core/event.h"
#include "zms/util/thread_pool.h"
#include "zms/media/container/flv/flv_index.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <sys/stat.h>
#define zms_http_fseek _fseeki64
#else
#include <strings.h>
#include <sys/stat.h>
#define zms_http_fseek fseeko
#endif


/*
 * 统一构造 HTTP 响应头。
 * 成功返回写入长度；格式化失败或超出缓冲上限返回 -1——调用方必须放弃本次响应，
 * 不能按返回值发送（截断的响应头会让客户端一直等，HLS 曾因此无法播放）。
 */
static int http_build_header(char *buf, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!buf || cap == 0 || !fmt) {
        return -1;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap) {
        zn_error("http: response header too long (n=%d cap=%zu)", n, cap);
        return -1;
    }
    return n;
}

void zms_http_response_send_error(zms_http_session *hs, int code, const char *reason)
{
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    int n = http_build_header(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: 0\r\n\r\n",
                     code, reason);
    if (n < 0) {
        /* 头构造失败（超限/格式化错误）：不能发送半截响应，直接断开 */
        if (hs->tcp) {
            zn_tcp_session_close(hs->tcp);
        }
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);
    zn_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

void zms_http_response_send_json(zms_http_session *hs, int status, const char *body,
                                 size_t body_len)
{
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    int n = http_build_header(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: application/json; charset=utf-8\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, status == 200 ? "OK" : "Error", body_len);
    if (n < 0) {
        /* 头构造失败（超限/格式化错误）：不能发送半截响应，直接断开 */
        if (hs->tcp) {
            zn_tcp_session_close(hs->tcp);
        }
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);
    if (body_len) {
        zn_tcp_session_send(hs->tcp, body, body_len);
    }
    zn_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

static const char *http_reason_phrase(int status)
{
    switch (status) {
    case 200:
        return "OK";
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 206:
        return "Partial Content";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 406:
        return "Not Acceptable";
    case 500:
        return "Internal Server Error";
    case 503:
        return "Service Unavailable";
    default:
        return "";
    }
}

void zms_http_response_send_bytes(zms_http_session *hs, int status, const char *ctype,
                                  const void *body, size_t body_len)
{
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    int n = http_build_header(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type, If-Match, Authorization\r\n"
                     "Access-Control-Expose-Headers: Location\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, http_reason_phrase(status), ctype, body_len);
    if (n < 0) {
        /* 头构造失败（超限/格式化错误）：不能发送半截响应，直接断开 */
        if (hs->tcp) {
            zn_tcp_session_close(hs->tcp);
        }
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);
    if (body_len && body) {
        zn_tcp_session_send(hs->tcp, body, body_len);
    }
    zn_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

void zms_http_response_send_bytes_buf(zms_http_session *hs, int status, const char *ctype,
                                      zn_buf *body)
{
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    size_t body_len = body ? zn_buf_len(body) : 0;
    int n = http_build_header(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\n"
                     "Connection: close\r\n"
                     "Content-Type: %s\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Access-Control-Allow-Methods: GET, POST, PATCH, DELETE, OPTIONS\r\n"
                     "Access-Control-Allow-Headers: Content-Type, If-Match, Authorization\r\n"
                     "Access-Control-Expose-Headers: Location\r\n"
                     "Cache-Control: no-cache\r\n"
                     "Content-Length: %zu\r\n\r\n",
                     status, http_reason_phrase(status), ctype, body_len);
    if (n < 0) {
        /* 头构造失败（超限/格式化错误）：不能发送半截响应，直接断开 */
        if (hs->tcp) {
            zn_tcp_session_close(hs->tcp);
        }
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);
    if (body_len > 0) {
        zn_tcp_session_send_buf(hs->tcp, body);
    }
    zn_tcp_session_flush(hs->tcp);
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

/*
 * 解析 HTTP Range 头，得到 [start, end] 闭区间。
 *
 * 支持的两种形式（RFC 7233）：
 *   bytes=100-200  显式区间
 *   bytes=100-     从 100 到末尾
 *   bytes=-500     最后 500 字节（注意这里是“后缀长度”而不是起点）
 *
 * 返回 1 表示按 Range 返回 206；0 表示忽略 Range、返回完整内容 200。
 * 返回 0 的几种情况都是刻意的保守处理：
 *   - 没有 Range 头 / 文件大小为 0
 *   - 语法不认识（例如多个区间，协议允许但这里不支持，返回全文更安全）
 *   - 区间越界或 a > b（非法请求，回退全文而不是 416，兼容老旧播放器）
 *
 * start/end 在函数入口就置为“整个文件”，因此调用方即使拿到 0 也能直接用。
 */
static int parse_http_range(const char *range, uint64_t file_size, uint64_t *start, uint64_t *end)
{
    const char *p;
    unsigned long long a = 0;
    unsigned long long b = 0;

    if (start) {
        *start = 0;
    }
    if (end) {
        *end = file_size > 0 ? file_size - 1 : 0;
    }
    if (!range || !range[0] || file_size == 0) {
        return 0;
    }
    p = strstr(range, "bytes=");
    if (!p) {
        return 0;
    }
    p += 6;
    if (*p == '-') {
        a = strtoull(p + 1, NULL, 10);
        if (a >= file_size) {
            a = 0;
        } else {
            a = file_size - a;
        }
        b = file_size - 1;
    } else {
        a = strtoull(p, NULL, 10);
        p = strchr(p, '-');
        if (p && p[1]) {
            b = strtoull(p + 1, NULL, 10);
        } else {
            b = file_size - 1;
        }
    }
    if (b >= file_size) {
        b = file_size - 1;
    }
    if (a > b) {
        return 0;
    }
    if (start) {
        *start = (uint64_t)a;
    }
    if (end) {
        *end = (uint64_t)b;
    }
    return 1;
}

typedef struct zms_http_vod_file_open_ctx {
    zms_http_session *hs;
    zms_media_source *src;
    char path[512];
    char range_hdr[256];
    uint64_t query_seek_ms;
    int head_only;
    int is_mp4;
    int prep_ok;
    int http_code;
    const char *http_reason;
    FILE *fp;
    uint64_t file_size;
    uint64_t start;
    uint64_t end;
    int has_range;
} zms_http_vod_file_open_ctx;

static void http_file_done(zms_http_session *hs);
static void flush_file(zms_http_session *hs);

/*
 * 后台线程侧：打开点播文件、取大小、算 Range/seek 参数。
 *
 * 整个 open + stat 都必须在工作线程完成——文件可能在网络盘上，stat 也能
 * 阻塞数百毫秒。结果通过 ctx->prep_ok / http_code 带回 poller，
 * 由 vod_file_open_prep_finish 决策是发文件还是回错误码。
 */
static void vod_file_open_prep_blocking(void *user)
{
    zms_http_vod_file_open_ctx *ctx = (zms_http_vod_file_open_ctx *)user;
    struct stat st;

    ctx->prep_ok = 0;
    ctx->fp = NULL;
    if (stat(ctx->path, &st) != 0 || st.st_size <= 0) {
        ctx->http_code = 404;
        ctx->http_reason = "Not Found";
        return;
    }
    ctx->file_size = (uint64_t)st.st_size;
    ctx->end = ctx->file_size - 1;
    ctx->start = 0;
    ctx->has_range = 0;

    if (ctx->is_mp4) {
        if (ctx->range_hdr[0]) {
            ctx->has_range =
                parse_http_range(ctx->range_hdr, ctx->file_size, &ctx->start, &ctx->end);
        }
        if (!ctx->head_only) {
            ctx->fp = fopen(ctx->path, "rb");
            if (!ctx->fp) {
                ctx->http_code = 404;
                ctx->http_reason = "Not Found";
                return;
            }
            if (ctx->has_range && zms_http_fseek(ctx->fp, (long long)ctx->start, SEEK_SET) != 0) {
                fclose(ctx->fp);
                ctx->fp = NULL;
                ctx->http_code = 416;
                ctx->http_reason = "Range Not Satisfiable";
                return;
            }
        }
        ctx->prep_ok = 1;
        return;
    }

    if (ctx->file_size < 256) {
        ctx->http_code = 404;
        ctx->http_reason = "Not Found";
        return;
    }

    if (ctx->query_seek_ms > 0 && (!ctx->range_hdr[0])) {
        const zms_vod_flv_index *idx = zms_vod_source_flv_index(ctx->src);
        ctx->start = (uint64_t)zms_vod_flv_index_byte_at_ms(idx, ctx->query_seek_ms);
        if (ctx->start >= ctx->file_size) {
            ctx->start = 0;
        }
        ctx->has_range = 1;
    } else if (ctx->range_hdr[0]) {
        ctx->has_range = parse_http_range(ctx->range_hdr, ctx->file_size, &ctx->start, &ctx->end);
    }

    ctx->fp = fopen(ctx->path, "rb");
    if (!ctx->fp) {
        ctx->http_code = 404;
        ctx->http_reason = "Not Found";
        return;
    }
    if (zms_http_fseek(ctx->fp, (long long)ctx->start, SEEK_SET) != 0) {
        fclose(ctx->fp);
        ctx->fp = NULL;
        ctx->http_code = 416;
        ctx->http_reason = "Range Not Satisfiable";
        return;
    }
    ctx->prep_ok = 1;
}

static void http_send_vod_mp4_finish(zms_http_session *hs, zms_media_source *src, int head_only,
                                     FILE *fp, uint64_t file_size, uint64_t start, uint64_t end,
                                     int has_range)
{
    uint64_t body_len;
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    int hn;

    if (src) {
     if (!zms_http_session_authorize_play(hs, "http-mp4", src)) {
            if (fp) {
                fclose(fp);
            }
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
        zms_media_source_reader_add(src);
        hs->play_start_ms = zn_monotonic_ms();
        zms_media_event_play(src, "http-mp4");
        hs->reader_attached = 1;
        hs->play_event = "http-mp4";
        hs->source = src;
    }

    body_len = has_range ? (end - start + 1) : file_size;

    if (has_range) {
        hn = http_build_header(hdr, sizeof(hdr),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/mp4\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Range: bytes %llu-%llu/%llu\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)start, (unsigned long long)end,
                      (unsigned long long)file_size, (unsigned long long)body_len);
    } else {
        hn = http_build_header(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/mp4\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)file_size);
    }
    if (hn < 0) {
        if (fp) {
            fclose(fp);
        }
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    zn_tcp_session_flush(hs->tcp);

    if (head_only) {
        http_file_done(hs);
        return;
    }

    hs->file_fp = fp;
    hs->file_remain = body_len;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}

static void http_send_vod_flv_finish(zms_http_session *hs, zms_media_source *src, FILE *fp,
                                     uint64_t file_size, uint64_t start, uint64_t end,
                                     int has_range)
{
    uint64_t body_len = (size_t)(end - start + 1);
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    int hn;

    {


        if (!zms_http_session_authorize_play(hs, "http-flv", src)) {
            fclose(fp);
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    zms_media_source_reader_add(src);
    hs->play_start_ms = zn_monotonic_ms();
    zms_media_event_play(src, "http-flv");
    hs->reader_attached = 1;
    hs->play_event = "http-flv";
    hs->source = src;

    if (has_range) {
        hn = http_build_header(hdr, sizeof(hdr),
                      "HTTP/1.1 206 Partial Content\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/x-flv\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Range: bytes %llu-%llu/%llu\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)start, (unsigned long long)end,
                      (unsigned long long)file_size, (unsigned long long)body_len);
    } else {
        hn = http_build_header(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Connection: close\r\n"
                      "Content-Type: video/x-flv\r\n"
                      "Accept-Ranges: bytes\r\n"
                      "Access-Control-Allow-Origin: *\r\n"
                      "Content-Length: %llu\r\n\r\n",
                      (unsigned long long)file_size);
        body_len = file_size;
        fseek(fp, 0, SEEK_SET);
    }
    if (hn < 0) {
        if (fp) {
            fclose(fp);
        }
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    zn_tcp_session_flush(hs->tcp);
    hs->file_fp = fp;
    hs->file_remain = body_len;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}

/*
 * 在 poller 上处理后台打开结果。
 *
 * 注意 free_ctx 参数：正常回调路径会 free，但会话已死时外层可能已经释放过，
 * 这里必须尊重调用方传入的标志，不能无条件 free（否则 double free）。
 */
static void vod_file_open_prep_finish(zms_http_vod_file_open_ctx *ctx, int free_ctx)
{
    zms_http_session *hs = ctx->hs;

    if (!zms_http_session_is_live(hs)) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
    } else if (!ctx->prep_ok) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
        zms_http_response_send_error(hs, ctx->http_code ? ctx->http_code : 404,
                                     ctx->http_reason ? ctx->http_reason : "Not Found");
    } else if (ctx->is_mp4) {
        http_send_vod_mp4_finish(hs, ctx->src, ctx->head_only, ctx->fp, ctx->file_size, ctx->start,
                                 ctx->end, ctx->has_range);
    } else {
        http_send_vod_flv_finish(hs, ctx->src, ctx->fp, ctx->file_size, ctx->start, ctx->end,
                                 ctx->has_range);
    }
    zms_http_session_async_release(hs);
    if (free_ctx) {
        free(ctx);
    }
}

static void vod_file_open_prep_on_io(void *user)
{
    vod_file_open_prep_finish((zms_http_vod_file_open_ctx *)user, 1);
}

static int vod_file_open_async(zms_http_session *hs, zms_media_source *src, const char *path,
                               const char *range_hdr, uint64_t query_seek_ms, int head_only,
                               int is_mp4)
{
    zn_poller *pol;
    zms_http_vod_file_open_ctx *ctx;

    if (!zms_vod_thread_pool_enabled() || !hs || !path || !path[0]) {
        return 0;
    }
    pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    ctx = (zms_http_vod_file_open_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return 0;
    }
    ctx->hs = hs;
    ctx->src = src;
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    if (range_hdr && range_hdr[0]) {
        strncpy(ctx->range_hdr, range_hdr, sizeof(ctx->range_hdr) - 1);
    }
    ctx->query_seek_ms = query_seek_ms;
    ctx->head_only = head_only;
    ctx->is_mp4 = is_mp4;

    zms_http_session_async_hold(hs);
    if (zms_vod_thread_pool_run(pol, vod_file_open_prep_blocking, vod_file_open_prep_on_io, ctx) !=
        ZN_OK) {
        zms_http_session_async_release(hs);
        free(ctx);
        return 0;
    }
    return 1;
}

typedef struct zms_http_static_file_ctx {
    zms_http_session *hs;
    zms_media_source *src;
    char path[512];
    char ctype[96];
    char player[32];
    int prep_ok;
    FILE *fp;
    uint64_t file_size;
} zms_http_static_file_ctx;

static void http_send_static_finish(zms_http_session *hs, zms_media_source *src, const char *ctype,
                                    const char *player, FILE *fp, uint64_t file_size)
{
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    int hn;

    if (src) {
     if (!zms_http_session_authorize_play(hs, player, src)) {
            fclose(fp);
            zms_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
        zms_media_source_reader_add(src);
        hs->play_start_ms = zn_monotonic_ms();
        zms_media_event_play(src, player);
        hs->reader_attached = 1;
        hs->play_event = player;
        hs->source = src;
    }
    hn = http_build_header(hdr, sizeof(hdr),
                  "HTTP/1.1 200 OK\r\n"
                  "Connection: close\r\n"
                  "Content-Type: %s\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Content-Length: %llu\r\n\r\n",
                  ctype, (unsigned long long)file_size);
    if (hn < 0) {
        if (fp) {
            fclose(fp);
        }
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    zn_tcp_session_flush(hs->tcp);
    hs->file_fp = fp;
    hs->file_remain = file_size;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}

static void static_file_prep_blocking(void *user)
{
    zms_http_static_file_ctx *ctx = (zms_http_static_file_ctx *)user;
    struct stat st;

    ctx->prep_ok = 0;
    ctx->fp = NULL;
    if (stat(ctx->path, &st) != 0 || st.st_size <= 0) {
        return;
    }
    ctx->file_size = (uint64_t)st.st_size;
    ctx->fp = fopen(ctx->path, "rb");
    if (!ctx->fp) {
        return;
    }
    ctx->prep_ok = 1;
}

static void static_file_prep_finish(zms_http_static_file_ctx *ctx, int free_ctx)
{
    zms_http_session *hs = ctx->hs;

    if (!zms_http_session_is_live(hs)) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
    } else if (!ctx->prep_ok) {
        if (ctx->fp) {
            fclose(ctx->fp);
        }
        zms_http_response_send_error(hs, 404, "Not Found");
    } else {
        http_send_static_finish(hs, ctx->src, ctx->ctype, ctx->player, ctx->fp, ctx->file_size);
    }
    zms_http_session_async_release(hs);
    if (free_ctx) {
        free(ctx);
    }
}

static void static_file_prep_on_io(void *user)
{
    static_file_prep_finish((zms_http_static_file_ctx *)user, 1);
}

static int static_file_open_async(zms_http_session *hs, zms_media_source *src, const char *path,
                                  const char *ctype, const char *player)
{
    zn_poller *pol;
    zms_http_static_file_ctx *ctx;

    if (!zms_vod_thread_pool_enabled() || !hs || !path || !path[0]) {
        return 0;
    }
    pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    ctx = (zms_http_static_file_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        return 0;
    }
    ctx->hs = hs;
    ctx->src = src;
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);
    if (ctype && ctype[0]) {
        strncpy(ctx->ctype, ctype, sizeof(ctx->ctype) - 1);
    }
    if (player && player[0]) {
        strncpy(ctx->player, player, sizeof(ctx->player) - 1);
    }

    zms_http_session_async_hold(hs);
    if (zms_vod_thread_pool_run(pol, static_file_prep_blocking, static_file_prep_on_io, ctx) !=
        ZN_OK) {
        zms_http_session_async_release(hs);
        free(ctx);
        return 0;
    }
    return 1;
}

/* 关闭文件放进 IO 池：poller 上不做 fclose。 */
typedef struct zms_http_file_close_job {
    FILE *fp;
} zms_http_file_close_job;

/* 后台线程侧：真正关闭文件。poller 回调只负责释放 job，不做 IO。 */
static void file_close_work(void *user)
{
    zms_http_file_close_job *j = (zms_http_file_close_job *)user;

    if (j && j->fp) {
        fclose(j->fp);
        j->fp = NULL;
    }
}

static void file_close_on_io(void *user)
{
    free(user);
}

/*
 * 异步关闭文件句柄。
 *
 * fclose 会 flush 缓冲并可能阻塞（尤其网络盘），不能放在 poller 上。
 * 三处降级到同步 fclose：没有线程池 / 没有 poller / 分配 job 失败——
 * 这些情况下宁可短暂阻塞，也不能泄漏句柄。
 *
 * 注意：本函数不触碰会话，因此调用方无需持有 hs；会话销毁路径也能安全调用。
 */
void zms_http_close_file_async(FILE *fp, zn_poller *pol)
{
    zms_http_file_close_job *j;

    if (!fp) {
        return;
    }
    if (!zms_vod_thread_pool_enabled() || !pol) {
        fclose(fp); /* 无异步环境：只能就地关闭（降级路径） */
        return;
    }
    j = (zms_http_file_close_job *)calloc(1, sizeof(*j));
    if (!j) {
        fclose(fp);
        return;
    }
    j->fp = fp;
    if (zms_vod_thread_pool_run(pol, file_close_work, file_close_on_io, j) != ZN_OK) {
        fclose(fp);
        free(j);
    }
}

/** 异步读一块到会话预读缓冲；poller 上只发。
 *
 * 设计要点：读与发彻底分离。文件读取在工作线程完成，结果（j->got 字节）在
 * poller 上换进 hs->file_rd_buf，再由 flush_file 走非阻塞发送。
 * 这样读盘再慢，也只是让这一路会话变慢，不会阻塞同 poller 的其它会话。
 */
typedef struct zms_http_file_read_job {
    zms_http_session *hs;
    FILE *fp;
    uint8_t *buf;
    size_t want;
    size_t got;
} zms_http_file_read_job;

static void file_read_work(void *user)
{
    zms_http_file_read_job *j = (zms_http_file_read_job *)user;

    j->got = (j->fp && j->buf && j->want) ? fread(j->buf, 1, j->want, j->fp) : 0;
}

/*
 * 读完成回调（poller 线程）。
 *
 * got==0 视为 EOF：文件读完了，但要留意这可能只是本次 want 为 0 的退化情况，
 * 所以 file_read_submit 在 want==0 时就直接置 EOF，不会走到这里。
 *
 * 释放顺序：先 async_release 再 flush_file。release 可能触发会话彻底释放，
 * 而 flush_file 需要 hs 存活——因此两者顺序不能反，且 release 之前已经把
 * 需要的数据搬进 hs。
 */
static void file_read_on_io(void *user)
{
    zms_http_file_read_job *j = (zms_http_file_read_job *)user;
    zms_http_session *hs = j->hs;

    if (hs) {
        hs->file_reading = 0;
    }
    if (!zms_http_session_is_live(hs)) {
        free(j->buf);
        zms_http_session_async_release(hs);
        free(j);
        return;
    }
    if (j->got == 0) {
        hs->file_eof = 1;
    } else {
        free(hs->file_rd_buf);
        hs->file_rd_buf = j->buf;
        j->buf = NULL;
        hs->file_rd_len = j->got;
        hs->file_rd_off = 0;
        hs->file_remain = hs->file_remain > j->got ? hs->file_remain - j->got : 0;
    }
    zms_http_session_async_release(hs);
    free(j->buf);
    free(j);
    flush_file(hs);
}

/*
 * 提交一次异步读。want 取 min(剩余长度, 发送缓冲容量)：
 * 读得比发送缓冲还大没有意义，反而要临时再拷一次。
 *
 * 任何一步失败（无 poller / 线程池关闭 / 分配失败 / 投递失败）都置 file_eof，
 * 让上层按“读完”收尾并关闭会话——这里没有中间态可选，卡住比断开更糟。
 */
static void file_read_submit(zms_http_session *hs)
{
    zms_http_file_read_job *j;
    zn_poller *pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    size_t want = hs->file_remain > hs->send_cap ? hs->send_cap : (size_t)hs->file_remain;

    if (!pol || !zms_vod_thread_pool_enabled() || want == 0) {
        hs->file_eof = 1;
        return;
    }
    j = (zms_http_file_read_job *)calloc(1, sizeof(*j));
    if (!j) {
        hs->file_eof = 1;
        return;
    }
    j->buf = (uint8_t *)malloc(want);
    if (!j->buf) {
        free(j);
        hs->file_eof = 1;
        return;
    }
    j->hs = hs;
    j->fp = hs->file_fp;
    j->want = want;
    hs->file_reading = 1;
    zms_http_session_async_hold(hs);
    if (zms_vod_thread_pool_run(pol, file_read_work, file_read_on_io, j) != ZN_OK) {
        hs->file_reading = 0;
        zms_http_session_async_release(hs);
        free(j->buf);
        free(j);
        hs->file_eof = 1;
    }
}

static void http_file_done(zms_http_session *hs)
{
    zn_poller *pol;

    if (!hs) {
        return;
    }
    pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    if (hs->file_fp) {
        zms_http_close_file_async(hs->file_fp, pol);
        hs->file_fp = NULL;
    }
    free(hs->file_rd_buf);
    hs->file_rd_buf = NULL;
    hs->file_rd_len = 0;
    hs->file_rd_off = 0;
    hs->file_eof = 0;
    hs->file_remain = 0;
    if (hs->reader_attached && hs->source) {
        zms_media_source_reader_remove(hs->source);
        zms_media_event_stop(hs->source, hs->play_event ? hs->play_event : "http-flv",
                             hs->play_start_ms);
        hs->reader_attached = 0;
        hs->play_start_ms = 0;
    }
    hs->play_event = NULL;
    hs->source = NULL;
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

static void flush_file(zms_http_session *hs)
{
    int i;

    if (!hs || hs->state != ZMS_HTTP_SESSION_STATE_FILE_SENDING || !hs->file_fp || !hs->tcp) {
        return;
    }

    /* poller 只做「发送预读好的数据」；没有可发数据时提交一次异步读。 */
    for (i = 0; i < 32; ++i) {
        if (zn_tcp_session_out_pending(hs->tcp) > 256 * 1024) {
            break;
        }
        zn_tcp_session_flush(hs->tcp);

        if (hs->file_rd_off < hs->file_rd_len) {
            size_t n = hs->file_rd_len - hs->file_rd_off;

            zn_tcp_session_send(hs->tcp, hs->file_rd_buf + hs->file_rd_off, n);
            hs->file_rd_off = hs->file_rd_len;
            continue;
        }
        if (hs->file_remain > 0 && !hs->file_reading && !hs->file_eof) {
            file_read_submit(hs);
        }
        break;
    }
    zn_tcp_session_flush(hs->tcp);

    if (hs->file_remain == 0 && !hs->file_reading && hs->file_rd_off >= hs->file_rd_len) {
        http_file_done(hs);
    }
}

/*
 * HLS 分片/清单发送完成后的收尾。
 *
 * HLS 请求通常很短命（一次 GET 一个切片），所以发完就把会话状态复位为
 * IDLE 等待下一个 keep-alive 请求，而不是关闭连接——播放器会连续发很多
 * 切片请求，复用连接能显著降低握手开销。
 */
static void http_hls_send_done(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    hs->hls_send_len = 0;
    hs->hls_send_off = 0;
    if (hs->tcp) {
        zn_tcp_session_close(hs->tcp);
    }
    hs->state = ZMS_HTTP_SESSION_STATE_IDLE;
}

/*
 * 发送 HLS 响应头并进入 body 发送状态。
 *
 * 这里正是历史上 HLS 卡死的现场：Content-Type/长度 + CORS 头拼起来一度
 * 超过缓冲上限，头被截断，播放器拿不到完整 Content-Length 就一直等。
 * 现在头构造失败会直接断开（见调用方对返回值的处理），不会留下半截响应。
 */
static void start_hls_body_send(zms_http_session *hs, const char *ctype, size_t body_len)
{
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    int n;

    if (!hs || !hs->tcp || body_len == 0) {
        return;
    }
    n = http_build_header(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Connection: close\r\n"
                 "Content-Type: %s\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Cache-Control: no-cache\r\n"
                 "Content-Length: %zu\r\n\r\n",
                 ctype, body_len);
    if (n < 0) {
        /* 头构造失败（超限/格式化错误）：不能发送半截响应，直接断开 */
        if (hs->tcp) {
            zn_tcp_session_close(hs->tcp);
        }
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);
    hs->hls_send_off = 0;
    hs->hls_send_len = body_len;
    hs->state = ZMS_HTTP_SESSION_STATE_HLS_SENDING;
    zms_http_session_flush(hs);
}

static void flush_hls(zms_http_session *hs)
{
    size_t chunk;
    size_t remain;
    int i;

    if (!hs || hs->state != ZMS_HTTP_SESSION_STATE_HLS_SENDING || !hs->tcp || !hs->send_buf) {
        return;
    }

    for (i = 0; i < 32; ++i) {
        remain = hs->hls_send_len > hs->hls_send_off ? hs->hls_send_len - hs->hls_send_off : 0;
        if (remain == 0) {
            break;
        }
        if (zn_tcp_session_out_pending(hs->tcp) > 256 * 1024) {
            break;
        }
        zn_tcp_session_flush(hs->tcp);

        chunk = remain > 256 * 1024 ? 256 * 1024 : remain;
        zn_tcp_session_send(hs->tcp, hs->send_buf + hs->hls_send_off, chunk);
        hs->hls_send_off += chunk;
    }
    zn_tcp_session_flush(hs->tcp);

    if (hs->hls_send_off >= hs->hls_send_len) {
        http_hls_send_done(hs);
    }
}

void zms_http_session_flush(zms_http_session *hs)
{
    if (!hs) {
        return;
    }
    if (hs->state == ZMS_HTTP_SESSION_STATE_STREAMING) {
        zms_http_session_stream_flush(hs);
    } else if (hs->state == ZMS_HTTP_SESSION_STATE_FILE_SENDING) {
        flush_file(hs);
    } else if (hs->state == ZMS_HTTP_SESSION_STATE_HLS_SENDING) {
        flush_hls(hs);
    }
}

void zms_http_response_send_static_file(zms_http_session *hs, zms_media_source *src,
                                        const char *path, const char *ctype, const char *player)
{
    zms_http_static_file_ctx sync;

    if (!hs || !path || !path[0]) {
        return;
    }
    if (static_file_open_async(hs, src, path, ctype, player)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.path, path, sizeof(sync.path) - 1);
    if (ctype && ctype[0]) {
        strncpy(sync.ctype, ctype, sizeof(sync.ctype) - 1);
    } else {
        strncpy(sync.ctype, "application/octet-stream", sizeof(sync.ctype) - 1);
    }
    if (player && player[0]) {
        strncpy(sync.player, player, sizeof(sync.player) - 1);
    }
    static_file_prep_blocking(&sync);
    static_file_prep_finish(&sync, 0);
}

void zms_http_response_send_vod_flv_file(zms_http_session *hs, zms_media_source *src,
                                         const char *flv_path, const char *range_hdr,
                                         uint64_t query_seek_ms)
{
    zms_http_vod_file_open_ctx sync;

    if (!hs || !flv_path || !flv_path[0]) {
        return;
    }
    if (vod_file_open_async(hs, src, flv_path, range_hdr, query_seek_ms, 0, 0)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.path, flv_path, sizeof(sync.path) - 1);
    if (range_hdr && range_hdr[0]) {
        strncpy(sync.range_hdr, range_hdr, sizeof(sync.range_hdr) - 1);
    }
    sync.query_seek_ms = query_seek_ms;
    vod_file_open_prep_blocking(&sync);
    vod_file_open_prep_finish(&sync, 0);
}

void zms_http_response_send_vod_mp4_file(zms_http_session *hs, zms_media_source *src,
                                         const char *mp4_path, const char *range_hdr, int head_only)
{
    zms_http_vod_file_open_ctx sync;

    if (!hs || !mp4_path || !mp4_path[0]) {
        return;
    }
    if (vod_file_open_async(hs, src, mp4_path, range_hdr, 0, head_only, 1)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.path, mp4_path, sizeof(sync.path) - 1);
    if (range_hdr && range_hdr[0]) {
        strncpy(sync.range_hdr, range_hdr, sizeof(sync.range_hdr) - 1);
    }
    sync.head_only = head_only;
    sync.is_mp4 = 1;
    vod_file_open_prep_blocking(&sync);
    vod_file_open_prep_finish(&sync, 0);
}

void zms_http_response_send_hls_body(zms_http_session *hs, const char *ctype, size_t body_len)
{
    start_hls_body_send(hs, ctype, body_len);
}

void zms_http_response_send_download_file(zms_http_session *hs, const char *path)
{
    struct stat st;
    FILE *fp;
    char hdr[ZMS_HTTP_RESP_HDR_MAX];
    const char *base;
    char fname[256];
    int hn;
    size_t i;

    if (!hs || !path || !path[0]) {
        return;
    }
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    fp = fopen(path, "rb");
    if (!fp) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    base = path;
    for (i = 0; path[i]; ++i) {
        if (path[i] == '/' || path[i] == '\\') {
            base = path + i + 1;
        }
    }
    strncpy(fname, base && base[0] ? base : "download.bin", sizeof(fname) - 1);
    fname[sizeof(fname) - 1] = '\0';
    hn = http_build_header(hdr, sizeof(hdr),
                  "HTTP/1.1 200 OK\r\n"
                  "Connection: close\r\n"
                  "Content-Type: application/octet-stream\r\n"
                  "Content-Disposition: attachment; filename=\"%s\"\r\n"
                  "Access-Control-Allow-Origin: *\r\n"
                  "Cache-Control: no-cache\r\n"
                  "Content-Length: %llu\r\n\r\n",
                  fname, (unsigned long long)st.st_size);
    if (hn < 0) {
        if (fp) {
            fclose(fp);
        }
        zms_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    zn_tcp_session_send(hs->tcp, hdr, (size_t)hn);
    zn_tcp_session_flush(hs->tcp);
    hs->file_fp = fp;
    hs->file_remain = (uint64_t)st.st_size;
    hs->state = ZMS_HTTP_SESSION_STATE_FILE_SENDING;
    flush_file(hs);
}
