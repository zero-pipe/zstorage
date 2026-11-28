/**
 * @file vod_http_handler.c
 * @brief VOD HTTP（FLV/HLS/MP4）服务与路由。
 *
 * 磁盘/转码类工作（构建 FLV 缓存、生成 HLS 播放列表/切片、读文件）都含
 * 阻塞 IO，绝不能跑在 HTTP poller 上。通用套路是三段式：
 *   - 在 poller 上做请求解析与资源判定，决定走“直接发文件”还是“异步准备”；
 *   - 阻塞部分投到 vod 线程池（zcs_vod_thread_pool_run 的 blocking 回调），
 *     期间用 zcs_http_session_async_hold 保活会话；
 *   - 完成回调 on_io 被投递回原 poller，此时才发响应、释放会话。
 * 每个异步 job 结构体都完整复制了所需参数（app/stream/path），因为会话可能
 * 在异步期间被别的请求改写。
 */
#include "zcs/play/http_port.h"
#include "session/http/session_internal.h"
#include "zn/platform.h"
#include "zcs/protocol/http/route.h"
#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include "zcs/core/event.h"
#include "zcs/core/stream/hub.h"
#include "zcs/util/thread_pool.h"
#include "zcs/media/container/flv/flv_index.h"
#include "zcs/vod/play/hls.h"
#include "zn/net/tcp_server.h"
#include "zn/poller/poller.h"
#include "zn/util/buf.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <sys/stat.h>


#endif

static zcs_media_source *http_find_play_src(const zcs_http_request *req, const char *app,
                                           const char *stream)
{
    char vhost[ZCS_VHOST_MAX];

    /* 播放源查找统一用 RTMP schema：VOD 文件被 loadMP4File 注册后就是挂在
     * RTMP 命名空间下的 media_source，HTTP 只是另一种取用方式。vhost 从请求
     * Host 解析，支持多租户。 */
    zcs_http_request_vhost(req, vhost, sizeof(vhost));
    return zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTMP, vhost, app, stream);
}

static uint64_t range_bytes_to_seek_ms(const char *range, uint64_t dur_ms, uint64_t est_bytes,
                                       const zcs_vod_flv_index *idx)
{
    const char *p;
    unsigned long long byte_start = 0;
    size_t i;

    if (!range || !range[0]) {
        return 0;
    }
    p = strstr(range, "bytes=");
    if (!p) {
        return 0;
    }
    p += 6;
    byte_start = strtoull(p, NULL, 10);
    if (byte_start == 0) {
        return 0;
    }

    /* 优先用 FLV 索引（keyframe 时间 ↔ 文件偏移）把字节位置精确映射成毫秒：
     * 找第一个 fileposition 越过 byte_start 的关键帧，返回其时间。 */
    if (idx && idx->count > 1) {
        for (i = 1; i < idx->count; ++i) {
            if (idx->filepositions[i] > (double)byte_start) {
                return (uint64_t)(idx->times[i] * 1000.0 + 0.5);
            }
        }
        if (dur_ms > 0) {
            return dur_ms > 1000 ? dur_ms - 1000 : 0;
        }
    }

    /* 无索引时按“字节比例 ≈ 时间比例”线性估算。若 byte_start 超出估计的文件
     * 大小（客户端 seek 越界），钳到 95% 处，留出尾部余量而不会落到 0。 */
    if (!dur_ms) {
        return 0;
    }
    if (!est_bytes) {
        est_bytes = dur_ms * 125ULL;
    }
    if (byte_start >= est_bytes) {
        byte_start = (unsigned long long)(est_bytes * 95ULL / 100ULL);
    }
    return (uint64_t)((double)byte_start / (double)est_bytes * (double)dur_ms);
}

static uint64_t vod_est_seek_bytes(const char *app, const char *stream, uint64_t dur_ms)
{
    char path[512];
    struct stat st;

    /* 拿真实文件大小最好；拿不到就按 dur_ms*125 估（125 字节/毫秒 ≈ 1Mbps，
     * 是常见点播码率下字节↔时间换算的经验值，只用于 seek 粗定位）。 */
    if (zcs_vod_resolve_file_path(app, stream, path, sizeof(path)) == 0 && stat(path, &st) == 0 &&
        st.st_size > 0) {
        return (uint64_t)st.st_size;
    }
    return dur_ms > 0 ? dur_ms * 125ULL : 0;
}

static uint64_t vod_flv_resolve_seek_ms(zcs_media_source *src, const char *app, const char *stream,
                                        uint64_t seek_ms, const char *range_hdr)
{
    /* 已有显式 seek 或没有 Range 头时原样返回；只有“靠 Range 表达起点”的
     * 情况下才需要把字节偏移换算成毫秒。这层包裹让上方多处调用只需关心
     * “算不算”，不必重复判断。 */
    if (seek_ms || !range_hdr || !range_hdr[0]) {
        return seek_ms;
    }

    {
        const zcs_vod_flv_index *flv_idx = zcs_vod_source_flv_index(src);
        uint64_t vod_dur_ms = zcs_vod_source_duration_ms(src);
        uint64_t est_bytes;

        if (!vod_dur_ms) {
            vod_dur_ms = zcs_vod_probe_duration_ms(app, stream);
        }
        est_bytes = flv_idx && flv_idx->filesize > 0 ? (uint64_t)flv_idx->filesize
                                                     : vod_est_seek_bytes(app, stream, vod_dur_ms);
        return range_bytes_to_seek_ms(range_hdr, vod_dur_ms, est_bytes, flv_idx);
    }
}

typedef struct zcs_vod_flv_cache_ctx {
    zcs_http_session *hs;
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char flv_cache[512];
    char range_hdr[256];
    uint64_t seek_ms;
    zn_err_t cache_err;
} zcs_vod_flv_cache_ctx;

static zn_err_t vod_flv_start_hdr(void *muxer, int has_audio, int has_video, uint8_t *out,
                                   size_t cap, size_t *out_len)
{
    return zcs_vod_flv_muxer_start((zcs_vod_flv_muxer *)muxer, has_audio, has_video, out, cap,
                                   out_len);
}

static void vod_flv_cache_build_blocking(void *user)
{
    zcs_vod_flv_cache_ctx *job = (zcs_vod_flv_cache_ctx *)user;
    job->cache_err = zcs_vod_flv_cache_ensure(job->app, job->stream, job->src, NULL);
}

static void zcs_http_vod_flv_start_transcode(zcs_http_session *hs, zcs_media_source *src,
                                             const char *app, const char *stream, uint64_t seek_ms,
                                             const char *range_hdr);

static void vod_flv_cache_build_on_io(void *user)
{
    zcs_vod_flv_cache_ctx *job = (zcs_vod_flv_cache_ctx *)user;

    /* blocking 阶段已把全量 remux 成磁盘 FLV 缓存；回到 poller 后先确认会话
     * 仍存活（客户端可能已断开），再：缓存有效就按 Range/seek 发文件，
     * 否则退化为实时转码直推。注意 job 里存的是 src/app/stream 的快照，
     * 不依赖会话自身的字段。 */
    if (!zcs_http_session_is_live(job->hs)) {
        zcs_http_session_async_release(job->hs);
        free(job);
        return;
    }
    if (job->cache_err == ZN_OK && zcs_vod_flv_cache_valid(job->app, job->stream)) {
        zcs_http_response_send_vod_flv_file(job->hs, job->src, job->flv_cache,
                                            job->range_hdr[0] ? job->range_hdr : NULL,
                                            job->seek_ms);
    } else {
        uint64_t seek = vod_flv_resolve_seek_ms(job->src, job->app, job->stream, job->seek_ms,
                                                job->range_hdr[0] ? job->range_hdr : NULL);
        zcs_http_vod_flv_start_transcode(job->hs, job->src, job->app, job->stream, seek, NULL);
    }
    zcs_http_session_async_release(job->hs);
    free(job);
}

static void zcs_http_vod_flv_start_transcode(zcs_http_session *hs, zcs_media_source *src,
                                             const char *app, const char *stream, uint64_t seek_ms,
                                             const char *range_hdr)
{
    zn_poller *pol;
    uint64_t vod_dur_ms = 0;
    uint64_t play_ms = 0;
    size_t vcfg_len = 0;
    size_t acfg_len = 0;
    const uint8_t *vcfg = NULL;
    const uint8_t *acfg = NULL;
    char hdr[384];
    int n;

    pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        zcs_http_response_send_error(hs, 503, "Service Unavailable");
        return;
    }
    /* 打开 VOD 播放 lane（把文件读入内存环形缓冲 + 按需生成 muxer）。
     * seek 走 lane_seek_ms，否则 lane_prepare 从起点准备；prefill 预读一段，
     * 让首个响应不至于空等。 */
    hs->vod_lane = zcs_vod_play_lane_open(src, pol);
    if (!hs->vod_lane) {
        zn_warn("HTTP-FLV vod lane open failed: app=%s stream=%s", app, stream);
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (seek_ms) {
        play_ms = zcs_vod_play_lane_seek_ms(hs->vod_lane, seek_ms);
    } else {
        play_ms = zcs_vod_play_lane_prepare(hs->vod_lane, 0);
    }
    zcs_vod_play_lane_prefill(hs->vod_lane);
    vod_dur_ms = zcs_vod_source_duration_ms(src);
    if (!vod_dur_ms) {
        vod_dur_ms = zcs_vod_probe_duration_ms(app, stream);
    }
    vcfg = zcs_vod_play_lane_video_config(hs->vod_lane, &vcfg_len);
    acfg = zcs_vod_play_lane_audio_config(hs->vod_lane, &acfg_len);
    hs->vod_muxer =
        zcs_vod_flv_muxer_create_reader(src, zcs_vod_play_lane_buffer_reader(hs->vod_lane));
    if (!hs->vod_muxer) {
        zcs_vod_play_lane_close(hs->vod_lane);
        hs->vod_lane = NULL;
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }

    {
        const zcs_vod_flv_index *flv_idx = zcs_vod_source_flv_index(src);
        zcs_vod_flv_index_view *flv_view =
            flv_idx ? zcs_vod_flv_index_view_create(flv_idx, play_ms) : NULL;
        /* 把音视频 config、时长、FLV 索引视图塞进 muxer；索引视图按 play_ms
         * 裁剪，使 remux 出来的 FLV 时间戳从 seek 点附近的 keyframe 起算。
         * http_realtime_pace=1 让 muxer 按播放时钟节流输出（而非尽快 flush），
         * 与浏览器消费速度匹配，避免一次性灌满 socket。 */
        zcs_vod_flv_muxer_configure(hs->vod_muxer, vcfg, vcfg_len, acfg, acfg_len, vod_dur_ms);
        zcs_vod_flv_muxer_set_index_view(hs->vod_muxer, flv_view);
        zcs_vod_flv_muxer_set_http_realtime_pace(hs->vod_muxer, 1);
        zcs_vod_flv_muxer_seek(hs->vod_muxer,
                               play_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)play_ms);
    }
    zcs_vod_flv_muxer_bind_poller(hs->vod_muxer, pol);
    zcs_vod_flv_muxer_bind_source(hs->vod_muxer, src);
    hs->source = src;
    zn_info("HTTP-FLV vod play: app=%s stream=%s seek=%llu ms dur=%llu ms", app, stream,
             (unsigned long long)play_ms, (unsigned long long)vod_dur_ms);

    {


        if (!zcs_http_session_authorize_play(hs, "http-flv", src)) {
            zcs_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
    }
    zcs_media_source_reader_add(src);
    hs->play_start_ms = zn_monotonic_ms();
    zcs_media_event_play(src, "http-flv");
    hs->reader_attached = 1;
    hs->play_event = "http-flv";

    if (!vod_dur_ms) {
        vod_dur_ms = zcs_vod_probe_duration_ms(app, stream);
    }
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Connection: close\r\n"
                 "Content-Type: video/x-flv\r\n"
                 "Access-Control-Allow-Origin: *\r\n"
                 "Accept-Ranges: none\r\n"
                 "X-Vod-Duration-Ms: %llu\r\n"
                 "X-Vod-Seek-Query: start=<ms>|seek=<sec>\r\n\r\n",
                 (unsigned long long)vod_dur_ms);
    zn_tcp_session_send(hs->tcp, hdr, (size_t)n);

    zcs_http_flv_emit_header_and_stream(hs, src->has_audio, src->has_video, vod_flv_start_hdr,
                                        hs->vod_muxer);
}

void zcs_http_vod_flv_start(zcs_http_session *hs, zcs_media_source *src, const char *app,
                            const char *stream, uint64_t seek_ms, const char *range_hdr)
{
    zn_poller *pol;
    char flv_cache[512];
    char disk_flv[512];

    if (!hs || !src) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    /* 磁盘原生 .flv：直接 Range/文件下发（ZLMediaKit 路径），避免 demux→remux + Accept-Ranges: none */
    if (zcs_vod_stream_is_native_flv_file(stream) &&
        zcs_vod_resolve_file_path(app, stream, disk_flv, sizeof(disk_flv))) {
        if (!seek_ms && range_hdr && range_hdr[0]) {
            seek_ms = vod_flv_resolve_seek_ms(src, app, stream, 0, range_hdr);
        }
        zn_info("HTTP-FLV vod file: %s seek=%llu ms%s", disk_flv, (unsigned long long)seek_ms,
                 (range_hdr && range_hdr[0]) ? " (Range)" : "");
        zcs_http_response_send_vod_flv_file(hs, src, disk_flv, range_hdr, seek_ms);
        return;
    }

    /* 决策阶梯（从最省事到最费）：
     *   1. 磁盘原生 .flv —— 直接发（上面已返回）；
     *   2. 已有有效 FLV 缓存 —— 直接发缓存；
     *   3. 无缓存但可开线程池 —— 异步构建缓存后回到 on_io 再发；
     *   4. 同步构建缓存（线程池不可用的兜底）；
     *   5. 都不行 —— 实时转码直推。 */
    pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    if (pol && zcs_vod_flv_cache_path(app, stream, flv_cache, sizeof(flv_cache))) {
        if (zcs_vod_flv_cache_valid(app, stream)) {
            zcs_http_response_send_vod_flv_file(hs, src, flv_cache, range_hdr, seek_ms);
            return;
        }
        if (zcs_vod_thread_pool_enabled()) {
            zcs_vod_flv_cache_ctx *job = (zcs_vod_flv_cache_ctx *)calloc(1, sizeof(*job));
            if (job) {
                job->hs = hs;
                job->src = src;
                strncpy(job->app, app, sizeof(job->app) - 1);
                strncpy(job->stream, stream, sizeof(job->stream) - 1);
                strncpy(job->flv_cache, flv_cache, sizeof(job->flv_cache) - 1);
                if (range_hdr && range_hdr[0]) {
                    strncpy(job->range_hdr, range_hdr, sizeof(job->range_hdr) - 1);
                }
                job->seek_ms = seek_ms;
                zcs_http_session_async_hold(hs);
                if (zcs_vod_thread_pool_run(pol, vod_flv_cache_build_blocking,
                                            vod_flv_cache_build_on_io, job) == ZN_OK) {
                    return;
                }
                zcs_http_session_async_release(hs);
                free(job);
            }
        }
        if (zcs_vod_flv_cache_ensure(app, stream, src, pol) == ZN_OK &&
            zcs_vod_flv_cache_valid(app, stream)) {
            zcs_http_response_send_vod_flv_file(hs, src, flv_cache, range_hdr, seek_ms);
            return;
        }
    }

    seek_ms = vod_flv_resolve_seek_ms(src, app, stream, seek_ms, range_hdr);
    zcs_http_vod_flv_start_transcode(hs, src, app, stream, seek_ms, NULL);
}

static void http_vod_hls_attach_play(zcs_http_session *hs, zcs_media_source *src,
                                     const char *player)
{
    if (!hs || !src) {
        return;
    }
    /* 鉴权通过后登记一个“播放读者”：reader_add 增加源上的读者计数（影响
     * 源的续命/清理），并记录播放开始时间与事件用于统计。鉴权失败则直接
     * 不发会话计数（调用方负责发 403）。 */
    if (!zcs_http_session_authorize_play(hs, player, src)) {
        return;
    }
    zcs_media_source_reader_add(src);
    hs->play_start_ms = zn_monotonic_ms();
    zcs_media_event_play(src, player);
    hs->reader_attached = 1;
    hs->play_event = player;
    hs->source = src;
}

typedef struct vod_hls_m3u8_job {
    zcs_http_session *hs;
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char rel_path[256];
    char disk_path[512];
    int need_ensure;
    int ok;
    uint8_t *body;
    size_t body_len;
} vod_hls_m3u8_job;

typedef struct zcs_vod_hls_seg_ctx {
    zcs_http_session *hs;
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char play_stream[ZCS_STREAM_MAX];
    char rel_path[256];
    char disk_path[512];
    uint64_t seg_no;
    zn_err_t err;
} zcs_vod_hls_seg_ctx;

static void http_send_vod_hls_m3u8_finish(zcs_http_session *hs, zcs_media_source *src,
                                          const char *app, const char *rel_path,
                                          const char *disk_path, const void *body, size_t body_len)
{
    size_t n;

    if (!hs || !body || body_len == 0) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    /* 点播 m3u8 可能比初始缓冲大：按需扩容（重写时还要再长一点）。 */
    if (zcs_http_session_send_reserve(hs, body_len + 1) != 0) {
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    memcpy(hs->send_buf, body, body_len);
    hs->send_buf[body_len] = '\0';
    /* m3u8 里的切片路径是相对清单位置的，但播放器实际按 HTTP 路由请求；
     * rewrite 把清单内引用改写成本服务端的 URL 形式（并注入必要参数）。 */
    n = zcs_http_route_rewrite_vod_hls_m3u8((char *)hs->send_buf, hs->send_cap, app, rel_path);
    if (src) {
        if (!zcs_http_session_authorize_play(hs, "hls", src)) {
            zcs_http_response_send_error(hs, 403, "Forbidden");
            return;
        }
        http_vod_hls_attach_play(hs, src, "hls");
    }
    zn_info("HLS vod m3u8: app=%s stream=%s path=%s bytes=%u", app, src ? src->stream : "",
             disk_path, (unsigned)n);
    zcs_http_response_send_bytes(hs, 200, "application/vnd.apple.mpegurl", hs->send_buf, n);
}

static void vod_hls_m3u8_blocking(void *user)
{
    vod_hls_m3u8_job *job = (vod_hls_m3u8_job *)user;
    FILE *fp;
    size_t n;
    uint8_t *buf;
    struct stat st;

    job->ok = 0;
    job->body = NULL;
    job->body_len = 0;

    if (job->need_ensure && job->src) {
        /* need_ensure=1 表示清单可能尚不存在（首次请求 mp4 派生出 m3u8）：
         * 先在阻塞线程里生成播放列表，再重新解析实际磁盘路径。 */
        if (zcs_vod_hls_ensure_playlist(job->src) != ZN_OK) {
            return;
        }
        if (!zcs_vod_hls_resolve_playlist(job->app, job->stream, job->disk_path,
                                          sizeof(job->disk_path))) {
            return;
        }
    }

    if (stat(job->disk_path, &st) != 0 || st.st_size <= 0 || st.st_size >= 4 * 1024 * 1024) {
        /* 4MB 上限：点播 m3u8 是文本清单，正常几十 KB；超限大概率是路径指向
         * 了错误的大文件，拒绝而不是把它整个读进内存。 */
        return;
    }
    fp = fopen(job->disk_path, "rb");
    if (!fp) {
        return;
    }
    n = (size_t)st.st_size;
    buf = (uint8_t *)malloc(n);
    if (!buf) {
        fclose(fp);
        return;
    }
    if (fread(buf, 1, n, fp) != n) {
        fclose(fp);
        free(buf);
        return;
    }
    fclose(fp);
    job->body = buf;
    job->body_len = n;
    job->ok = 1;
}

static void vod_hls_m3u8_on_io(void *user)
{
    vod_hls_m3u8_job *job = (vod_hls_m3u8_job *)user;

    /* 回到 poller：会话若已断，丢弃已在阻塞线程读到的 body；否则交给统一
     * 的 finish（rewrite + 发响应）。job->body 是阻塞线程 malloc 的，所有
     * 分支都要 free。 */
    if (!zcs_http_session_is_live(job->hs)) {
        zcs_http_session_async_release(job->hs);
        free(job->body);
        free(job);
        return;
    }
    if (!job->ok) {
        zcs_http_response_send_error(job->hs, 404, "Not Found");
        zcs_http_session_async_release(job->hs);
        free(job->body);
        free(job);
        return;
    }
    http_send_vod_hls_m3u8_finish(job->hs, job->src, job->app, job->rel_path, job->disk_path,
                                  job->body, job->body_len);
    zcs_http_session_async_release(job->hs);
    free(job->body);
    free(job);
}

static int http_send_vod_hls_m3u8_async(zcs_http_session *hs, zcs_media_source *src,
                                        const char *app, const char *stream, const char *rel_path,
                                        const char *disk_path, int need_ensure)
{
    zn_poller *pol;
    vod_hls_m3u8_job *job;

    /* 返回 1=已异步接管（调用方别再同步处理），0=不可异步/提交失败（调用方
     * 需走同步兜底）。线程池关闭或拿不到 poller 时返回 0。 */
    if (!zcs_vod_thread_pool_enabled() || !hs) {
        return 0;
    }
    pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    job = (vod_hls_m3u8_job *)calloc(1, sizeof(*job));
    if (!job) {
        return 0;
    }
    job->hs = hs;
    job->src = src;
    job->need_ensure = need_ensure;
    strncpy(job->app, app, sizeof(job->app) - 1);
    if (stream && stream[0]) {
        strncpy(job->stream, stream, sizeof(job->stream) - 1);
    }
    strncpy(job->rel_path, rel_path, sizeof(job->rel_path) - 1);
    strncpy(job->disk_path, disk_path, sizeof(job->disk_path) - 1);

    zcs_http_session_async_hold(hs);
    if (zcs_vod_thread_pool_run(pol, vod_hls_m3u8_blocking, vod_hls_m3u8_on_io, job) != ZN_OK) {
        zcs_http_session_async_release(hs);
        free(job);
        return 0;
    }
    return 1;
}

static void http_send_vod_hls_m3u8(zcs_http_session *hs, zcs_media_source *src, const char *app,
                                   const char *rel_path, const char *disk_path)
{
    vod_hls_m3u8_job sync;

    /* 异步优先；返回 0 说明线程池不可用，就在当前 poller 上同步读（小文本
     * 文件，短暂阻塞可接受），保证功能不因缺少线程池而中断。 */
    if (http_send_vod_hls_m3u8_async(hs, src, app, NULL, rel_path, disk_path, 0)) {
        return;
    }

    memset(&sync, 0, sizeof(sync));
    sync.hs = hs;
    sync.src = src;
    strncpy(sync.app, app, sizeof(sync.app) - 1);
    strncpy(sync.rel_path, rel_path, sizeof(sync.rel_path) - 1);
    strncpy(sync.disk_path, disk_path, sizeof(sync.disk_path) - 1);
    vod_hls_m3u8_blocking(&sync);
    if (!sync.ok) {
        free(sync.body);
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    http_send_vod_hls_m3u8_finish(hs, src, app, rel_path, disk_path, sync.body, sync.body_len);
    free(sync.body);
}

static void vod_hls_seg_blocking(void *user)
{
    zcs_vod_hls_seg_ctx *job = (zcs_vod_hls_seg_ctx *)user;

    /* 切片生成是 CPU/IO 密集（可能要转封装源文件），放线程池；先在阻塞线程
     * 里确保切片存在，再解析出磁盘路径交回 on_io 发文件。 */
    job->err = ZN_ERR_INVALID;
    if (!job->src) {
        return;
    }
    job->err = zcs_vod_hls_ensure_segment(job->src, job->seg_no, NULL);
    if (job->err != ZN_OK) {
        return;
    }
    if (!zcs_vod_hls_segment_path(job->app, job->play_stream, job->seg_no, job->disk_path,
                                  sizeof(job->disk_path))) {
        job->err = ZN_ERR_INVALID;
    }
}

static void vod_hls_seg_on_io(void *user)
{
    zcs_vod_hls_seg_ctx *job = (zcs_vod_hls_seg_ctx *)user;

    if (!zcs_http_session_is_live(job->hs)) {
        zcs_http_session_async_release(job->hs);
        free(job);
        return;
    }
    if (job->err != ZN_OK || !job->disk_path[0]) {
        zn_warn("HLS vod segment gen failed: app=%s stream=%s rel=%s", job->app, job->play_stream,
                 job->rel_path);
        zcs_http_response_send_error(job->hs, 404, "Not Found");
        zcs_http_session_async_release(job->hs);
        free(job);
        return;
    }
    zn_info("HLS vod ts (generated): app=%s stream=%s rel=%s path=%s", job->app, job->play_stream,
             job->rel_path, job->disk_path);
    zcs_http_response_send_static_file(job->hs, job->src, job->disk_path, "video/mp2t", "hls");
    zcs_http_session_async_release(job->hs);
    free(job);
}

static int zcs_http_vod_hls_handle_segment_async(zcs_http_session *hs, zcs_media_source *src,
                                                const char *app, const char *play_stream,
                                                const char *rel_path, uint64_t seg_no)
{
    zn_poller *pol;
    zcs_vod_hls_seg_ctx *job;

    if (!zcs_vod_thread_pool_enabled() || !hs || !src) {
        return 0;
    }
    pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
    if (!pol) {
        return 0;
    }

    job = (zcs_vod_hls_seg_ctx *)calloc(1, sizeof(*job));
    if (!job) {
        return 0;
    }
    job->hs = hs;
    job->src = src;
    job->seg_no = seg_no;
    strncpy(job->app, app, sizeof(job->app) - 1);
    strncpy(job->play_stream, play_stream, sizeof(job->play_stream) - 1);
    strncpy(job->rel_path, rel_path, sizeof(job->rel_path) - 1);

    zcs_http_session_async_hold(hs);
    if (zcs_vod_thread_pool_run(pol, vod_hls_seg_blocking, vod_hls_seg_on_io, job) != ZN_OK) {
        zcs_http_session_async_release(hs);
        free(job);
        return 0;
    }
    return 1;
}

void zcs_http_vod_hls_handle(zcs_http_session *hs, const char *vhost, zcs_media_source *src,
                            const char *app, const char *stream, const char *rel_path)
{
    char disk_path[512];
    char play_stream[ZCS_STREAM_MAX];
    zn_poller *pol;
    size_t flen;
    const char *mp4_stream = stream;

    if (!hs || !app || !rel_path || !rel_path[0]) {
        return;
    }

    flen = strlen(rel_path);
    if (flen >= 5 && strcmp(rel_path + flen - 5, ".m3u8") == 0) {
        struct stat st;
        int have_file = 0;

        if (zcs_vod_resolve_rel_path(app, rel_path, disk_path, sizeof(disk_path)) &&
            stat(disk_path, &st) == 0 && st.st_size > 16) {
            have_file = 1;
            /* 分段策略变更（GEN 递增）后本模块生成的旧清单/旧 .ts 必须重建，
             * 否则会继续播放错误切片；外部静态包（无 GEN 标记）不受影响。 */
            if (src && zcs_vod_hls_playlist_stale(disk_path)) {
                have_file = 0;
            }
        }
        if (!have_file) {
            if (!mp4_stream || !mp4_stream[0] || !src) {
                zn_warn("HLS vod m3u8 404: app=%s rel=%s", app, rel_path);
                zcs_http_response_send_error(hs, 404, "Not Found");
                return;
            }
            if (http_send_vod_hls_m3u8_async(hs, src, app, mp4_stream, rel_path, disk_path, 1)) {
                return;
            }
            if (zcs_vod_hls_ensure_playlist(src) != ZN_OK ||
                !zcs_vod_hls_resolve_playlist(app, mp4_stream, disk_path, sizeof(disk_path))) {
                zn_warn("HLS vod m3u8 404: app=%s rel=%s", app, rel_path);
                zcs_http_response_send_error(hs, 404, "Not Found");
                return;
            }
        }
        http_send_vod_hls_m3u8(hs, src, app, rel_path, disk_path);
        return;
    }

    {
        const char *dot = strrchr(rel_path, '.');
        const char *base = rel_path;
        const char *slash = strrchr(rel_path, '/');
        unsigned long long seg_no = 0;

        if (!dot || strcmp(dot, ".ts") != 0) {
            zcs_http_response_send_error(hs, 404, "Not Found");
            return;
        }
        if (slash) {
            base = slash + 1;
        }

        play_stream[0] = '\0';
        if (mp4_stream && mp4_stream[0]) {
            strncpy(play_stream, mp4_stream, sizeof(play_stream) - 1);
        } else {
            /* 切片 URL 只带 "N.ts"，不含源名：从同目录的兄弟 m3u8 反推登录
             * 对应 mp4 流名，才能定位到 media_source。 */
            (void)zcs_vod_infer_stream_from_ts(app, rel_path, play_stream, sizeof(play_stream));
        }
        if (!src && play_stream[0]) {
            src = zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTMP, vhost, app, play_stream);
        }

        if (zcs_vod_hls_resolve_segment_file(app, play_stream, rel_path, disk_path,
                                             sizeof(disk_path))) {
            zn_info("HLS vod ts: app=%s rel=%s path=%s", app, rel_path, disk_path);
            zcs_http_response_send_static_file(hs, src, disk_path, "video/mp2t", "hls");
            return;
        }

        if (!src || !play_stream[0]) {
            zn_warn("HLS vod ts 404: app=%s rel=%s (no source)", app, rel_path);
            zcs_http_response_send_error(hs, 404, "Not Found");
            return;
        }

        pol = hs->tcp ? zn_tcp_session_poller(hs->tcp) : NULL;
        seg_no = strtoull(base, NULL, 10);
        if (zcs_http_vod_hls_handle_segment_async(hs, src, app, play_stream, rel_path, seg_no)) {
            return;
        }
        if (!pol || zcs_vod_hls_ensure_segment(src, seg_no, pol) != ZN_OK ||
            !zcs_vod_hls_segment_path(app, play_stream, seg_no, disk_path, sizeof(disk_path))) {
            zn_warn("HLS vod segment gen failed: app=%s stream=%s rel=%s", app, play_stream,
                     rel_path);
            zcs_http_response_send_error(hs, 404, "Not Found");
            return;
        }
        zn_info("HLS vod ts (generated): app=%s stream=%s rel=%s path=%s", app, play_stream,
                 rel_path, disk_path);
        zcs_http_response_send_static_file(hs, src, disk_path, "video/mp2t", "hls");
    }
}

void zcs_http_vod_mp4_handle(zcs_http_session *hs, zcs_media_source *src, const char *app,
                            const char *stream, const char *range_hdr, int head_only)
{
    char mp4_path[512];

    if (!hs || !src || !app || !stream) {
        return;
    }

    /* 优先使用源注册时登记的实际磁盘路径（loadMP4File 的 file_path 可能不在
     * VOD 根目录下），再由 {vod_root}/{app}/{stream} 规则兜底。 */
    if (!zcs_vod_source_file_path(src, mp4_path, sizeof(mp4_path)) &&
        !zcs_vod_resolve_file_path(app, stream, mp4_path, sizeof(mp4_path))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    zn_info("HTTP-MP4 vod: app=%s stream=%s path=%s%s", app, stream, mp4_path,
             head_only ? " (HEAD)" : "");
    zcs_http_response_send_vod_mp4_file(hs, src, mp4_path, range_hdr, head_only);
}

static int vod_hls_route_match(const zcs_http_request *req)
{
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char hls_file[128];

    return req &&
           zcs_http_route_parse_vod_hls_path(req->path, app, stream, hls_file, sizeof(hls_file));
}

static void vod_hls_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char vhost[ZCS_VHOST_MAX];
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    char hls_file[128];

    if (strcmp(req->method, "GET") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zcs_http_request_vhost(req, vhost, sizeof(vhost));
    if (!zcs_http_route_parse_vod_hls_path(req->path, app, stream, hls_file, sizeof(hls_file))) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = stream[0] ? http_find_play_src(req, app, stream) : NULL;
    if (!src && hls_file[0]) {
        size_t fl = strlen(hls_file);
        if (fl >= 3 && strcmp(hls_file + fl - 3, ".ts") == 0) {
            char inferred[ZCS_STREAM_MAX];
            /* segment URL only carries N.ts; recover the mp4 stream from the sibling m3u8 */
            if (zcs_vod_infer_stream_from_ts(app, hls_file, inferred, sizeof(inferred)) &&
                inferred[0]) {
                strncpy(stream, inferred, sizeof(stream) - 1);
                stream[sizeof(stream) - 1] = '\0';
                src = http_find_play_src(req, app, stream);
            }
        }
    }
    if (!src) {
        char disk[512];
        struct stat st;
        if (!zcs_vod_resolve_rel_path(app, hls_file, disk, sizeof(disk)) || stat(disk, &st) != 0 ||
            st.st_size <= 0) {
            if (!stream[0] || !zcs_vod_hls_has_static_pack(app, stream)) {
                zcs_http_response_send_error(hs, 404, "Not Found");
                return;
            }
        }
    }
    zcs_http_vod_hls_handle(hs, vhost, src, app, stream, hls_file);
}

static int vod_mp4_route_match(const zcs_http_request *req)
{
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];

    return req && zcs_http_route_parse_mp4_path(req->path, app, stream);
}

static void vod_mp4_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];

    if (!req || (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0)) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    if (!zcs_http_route_parse_mp4_path(req->path, app, stream)) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || !zcs_media_source_is_vod(src)) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    /* 同一会话可能已经在一个流/文件上（keep-alive 复用），先停掉旧流再开新
     * 请求，避免两个发送器争用同一 socket。 */
    if (hs->state == ZCS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZCS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZCS_HTTP_SESSION_STATE_HLS_SENDING) {
        zcs_http_session_stop_stream(hs);
    }
    zcs_http_vod_mp4_handle(hs, src, app, stream, zcs_http_req_header(req, "Range"),
                           strcmp(req->method, "HEAD") == 0);
}

/** 云录像直出：/record/.../*.mp4 → ./www/record/...（HTTP-MP4 + Range，无需 loadMP4File） */
static int record_file_mp4_route_match(const zcs_http_request *req)
{
    size_t n;
    size_t i;

    if (!req || !req->path || strncmp(req->path, "/record/", 8) != 0) {
        return 0;
    }
    n = strlen(req->path);
    if (n < 12) {
        return 0;
    }
    for (i = 0; i < 4; ++i) {
        char a = req->path[n - 4 + i];
        char b = ".mp4"[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != b) {
            return 0;
        }
    }
    return 1;
}

static void record_file_mp4_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    char disk[512];
    size_t n;

    if (!hs || !req || !req->path) {
        return;
    }
    if (strcmp(req->method, "GET") != 0 && strcmp(req->method, "HEAD") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    /* 防目录穿越：拒绝含 ".." 的路径（最粗暴但有效的越权防护）。 */
    if (strstr(req->path, "..") != NULL) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    n = (size_t)snprintf(disk, sizeof(disk), "./www%s", req->path);
    if (n == 0 || n >= sizeof(disk)) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    /* 即便没有 ".."，也再校验规范化后确实落在录像根目录下，防止符号链接/
     * 畸形路径绕过上面的字面检查。 */
    if (!zcs_mp4_recorder_path_under_root(disk)) {
        zcs_http_response_send_error(hs, 403, "Forbidden");
        return;
    }
    if (hs->state == ZCS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZCS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZCS_HTTP_SESSION_STATE_HLS_SENDING) {
        zcs_http_session_stop_stream(hs);
    }
    zn_info("HTTP-MP4 record file: %s%s", disk, strcmp(req->method, "HEAD") == 0 ? " (HEAD)" : "");
    zcs_http_response_send_vod_mp4_file(hs, NULL, disk, zcs_http_req_header(req, "Range"),
                                        strcmp(req->method, "HEAD") == 0);
}

static const zcs_http_route_ops k_vod_hls_route = {
    .name = "vod-hls",
    .match = vod_hls_route_match,
    .handle = vod_hls_route_handle,
};

static const zcs_http_route_ops k_vod_mp4_route = {
    .name = "vod-mp4",
    .match = vod_mp4_route_match,
    .handle = vod_mp4_route_handle,
};

static const zcs_http_route_ops k_record_file_mp4_route = {
    .name = "record-file-mp4",
    .match = record_file_mp4_route_match,
    .handle = record_file_mp4_route_handle,
};

static int vod_flv_route_match(const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (!req) {
        return 0;
    }
    zcs_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        return 0;
    }
    src = http_find_play_src(req, app, stream);
    return src && zcs_media_source_is_vod(src);
}

static void vod_flv_route_handle(zcs_http_session *hs, const zcs_http_request *req)
{
    zcs_media_source *src;
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    uint64_t seek_ms = 0;

    if (strcmp(req->method, "GET") != 0) {
        zcs_http_response_send_error(hs, 405, "Method Not Allowed");
        return;
    }
    zcs_http_route_parse_flv_path(req->path, app, stream, &seek_ms);
    if (!app[0] || !stream[0]) {
        zcs_http_response_send_error(hs, 400, "Bad Request");
        return;
    }
    src = http_find_play_src(req, app, stream);
    if (!src || !zcs_media_source_is_vod(src)) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    if (hs->state == ZCS_HTTP_SESSION_STATE_STREAMING ||
        hs->state == ZCS_HTTP_SESSION_STATE_FILE_SENDING ||
        hs->state == ZCS_HTTP_SESSION_STATE_HLS_SENDING) {
        zcs_http_session_stop_stream(hs);
    }
    zcs_http_vod_flv_start(hs, src, app, stream, seek_ms, zcs_http_req_header(req, "Range"));
}

static const zcs_http_route_ops k_vod_flv_route = {
    .name = "vod-flv",
    .match = vod_flv_route_match,
    .handle = vod_flv_route_handle,
};

void zcs_vod_http_routes_register(void)
{
    zcs_http_route_register(&k_vod_hls_route);
    zcs_http_route_register(&k_record_file_mp4_route);
    zcs_http_route_register(&k_vod_mp4_route);
    zcs_http_route_register(&k_vod_flv_route);
}
