/**
 * @file segmenter.c
 * @brief 直播 HLS(TS) 分片器。
 */
#include "zcs/play/hls/segmenter.h"
#include "zcs/play/hls/sidecar.h"
#include "zcs/media/codec/param_sets.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/container/container_registry.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/frame.h"
#include "zcs/media/clock.h"
#include "zcs/media/media_limits.h"
#include "zcs/egress/recorder/egress_segment_recorder.h"
#include "zn/util/buf.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include "zn/poller/poller.h"
#include "zb/os/sync.h"
#include "zn/platform.h"
#include "bridge/zm_zcs_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <direct.h>
#define HLS_MKDIR(p) _mkdir(p)
#else
#define HLS_MKDIR(p) mkdir((p), 0755)
#endif

#include "play/hls/segmenter_internal.h"
#include "play/hls/hls_io.h"


/* 文件名/路径段安全校验：拒绝空串与含 ".." 的输入。
 * app/stream/分片名最终拼进磁盘路径，若放任 ".." 就会被构造出目录穿越
 * （写到 save_path 之外）。分片名由内部生成，但 app/stream 来自客户端 URL。 */
static int hls_save_name_ok(const char *s)
{
    return s && s[0] && strstr(s, "..") == NULL;
}

/* 逐级创建目录（等价于 mkdir -p）。注意跳过 Windows 盘符这个伪分隔点：
 * "C:\\a\\b" 在 ':' 后紧跟反斜杠，若不特判会把 "C:" 当目录去 stat/mkdir。 */
static int hls_save_mkdir_p(const char *dir)
{
    char tmp[512];
    char *p;
    struct stat st;

    if (!dir || !dir[0]) {
        return 0;
    }
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') {
            continue;
        }
        char sep = *p;
        *p = '\0';
        if (stat(tmp, &st) != 0 && !(strlen(tmp) == 2 && tmp[1] == ':')) {
            (void)HLS_MKDIR(tmp);
        }
        *p = sep;
    }
    if (stat(tmp, &st) == 0) {
        return 1;
    }
    return HLS_MKDIR(tmp) == 0;
}

void hls_save_write_file(zcs_http_hls_segmenter *rec, const char *name, const void *data,
                         size_t len)
{
    if (!rec || !rec->save_enabled || !hls_save_name_ok(name) || !data || len == 0)
        return;
    /* 磁盘写入交给 HLS 单 worker FIFO，避免 poller 阻塞。 */
    zcs_hls_io_enqueue(rec->save_dir, name, data, len);
}

static void hls_save_playlist(zcs_http_hls_segmenter *rec)
{
    char text[4096];
    size_t n = 0;

    if (!rec || !rec->save_enabled || !rec->maker) {
        return;
    }
    if (zcs_http_hls_playlist_build_m3u8(rec->maker, text, sizeof(text), &n) != ZN_OK || n == 0) {
        return;
    }
    hls_save_write_file(rec, "hls.m3u8", text, n);
}

/*
 * TS 切片回调：每切出一个分片，包进引用计数 buf 交给 playlist 环。
 * 走 push_segment_buf 而不是拷贝正文，是为了让 playlist 直接持有分片内存，
 * 供 HTTP 引用式发送（ref_segment）零拷贝外出。
 *
 * duration 的钳制理由：packager 偶尔会给出 0 或异常的负/超大时长，若原样
 * 写入 m3u8 的 #EXTINF，播放器会据此计算出错误的缓冲策略甚至拒绝播放。
 * 统一回落到 2s（默认分片时长）比透传一个坏值更安全。
 */
int hls_media_segment_cb(void *param, const void *data, size_t bytes, int64_t pts_ms,
                         int64_t dts_ms, int64_t duration_ms)
{
    zcs_http_hls_segmenter *rec = (zcs_http_hls_segmenter *)param;
    zn_buf *buf;
    uint64_t dur_ms;

    if (!rec || !rec->maker || !data || bytes == 0) {
        return 0;
    }
    if (duration_ms < 0) {
        duration_ms = 0;
    }
    dur_ms = (uint64_t)duration_ms;
    if (dur_ms == 0) {
        dur_ms = 2000;
    }
    if (dur_ms > 60000u) {
        dur_ms = 2000;
    }

    buf = zn_buf_alloc(bytes);
    if (!buf) {
        return -1;
    }
    memcpy((void *)zn_buf_data(buf), data, bytes);
    zn_buf_set_len(buf, bytes);
    if (zcs_http_hls_playlist_push_segment_buf(rec->maker, buf, pts_ms, dts_ms, dur_ms, 0) !=
        ZN_OK) {
        zn_buf_unref(buf);
        return -1;
    }
    rec->ts_seg_open = 0;
    {
        char latest[64];

        if (rec->src &&
            zcs_http_hls_playlist_latest_segment(rec->maker, latest, sizeof(latest), NULL)) {
            zn_info("HLS live segment cut: %s/%s %s dur=%llu bytes=%u", rec->src->app,
                     rec->src->stream, latest, (unsigned long long)dur_ms, (unsigned)bytes);
            if (rec->save_enabled) {
                hls_save_write_file(rec, latest, data, bytes);
                hls_save_playlist(rec);
            }
        }
    }
    return 0;
}

static int hls_zm_on_init(void *ctx, const char *name, const void *data, size_t len)
{
    (void)ctx;
    (void)name;
    (void)data;
    (void)len;
    return 0;
}

/* zm packager 的分片回调 → 统一走 hls_media_segment_cb；pts 这里用 pts_ms 直接
 * 顶替 dts（packager 已按输出时间轴给出一致的 pts/dts）。清单仍由上层生成，
 * 故 on_playlist 为空实现。 */
static int hls_zm_on_segment(void *ctx, const char *name, const void *data, size_t len,
                              int64_t pts_ms, int64_t duration_ms, int discontinuity)
{
    (void)name;
    (void)discontinuity;
    return hls_media_segment_cb(ctx, data, len, pts_ms, pts_ms, duration_ms);
}

static int hls_zm_on_playlist(void *ctx, const char *name, const char *text, size_t len)
{
    (void)ctx;
    (void)name;
    (void)text;
    (void)len;
    return 0; /* 清单仍由 zcs_http_hls_playlist 生成 */
}

/*
 * 判断是否必须走 fMP4：MPEG-TS 在标准实践中只可靠承载 H.264/H.265(+AAC)，
 * VP8/VP9/AV1/H266 没有广泛兼容的 TS 封装，浏览器 SASL/原生 HLS 也只认
 * fMP4 里的这些编码。所以源一旦是这些 codec，就必须切 CMAF/fMP4 路径。
 */
static int hls_source_needs_fmp4(const zcs_media_source *src)
{
    if (!src || !src->video.ready) {
        return 0;
    }
    switch (src->video.codec) {
    case ZCS_CODEC_VP8:
    case ZCS_CODEC_VP9:
    case ZCS_CODEC_AV1:
    case ZCS_CODEC_H266:
        return 1;
    default:
        return 0;
    }
}

void zcs_http_hls_segmenter_default_opts(zcs_http_hls_segmenter_opts *opts)
{
    if (!opts) {
        return;
    }
    opts->segment_duration_sec = 2.f;
    opts->segment_count = 3;
    opts->enable_audio = 1;
    opts->force_fmp4 = 0;
}

/* HLS recorder 在 publish 时创建，此时 codec 可能尚未就绪，use_fmp4 会被误判为 0。
 * 首个 tick 若发现源其实是 AV1/VP8/VP9/H266，且还没有产出任何分片，就切到 fMP4。 */
static void hls_maybe_upgrade_to_fmp4(zcs_http_hls_segmenter *rec)
{
    zcs_http_hls_playlist_opts mopts;
    zcs_http_hls_playlist *maker;
    zcs_hls_fmp4 *fmp4;
    int64_t seg_ms;

    if (!rec || rec->use_fmp4 || !rec->maker || !rec->src) {
        return;
    }
    if (!hls_source_needs_fmp4(rec->src)) {
        return;
    }
    if (zcs_http_hls_playlist_segment_count(rec->maker) > 0) {
        return; /* 已产出 TS 分片，不再回切 */
    }

    seg_ms = (int64_t)(rec->seg_duration_sec * 1000.f);
    if (seg_ms <= 0) {
        seg_ms = 2000;
    }
    memset(&mopts, 0, sizeof(mopts));
    mopts.segment_duration_sec = rec->seg_duration_sec;
    mopts.segment_count = rec->seg_count;
    mopts.use_fmp4 = 1;
    maker = zcs_http_hls_playlist_create(&mopts);
    if (!maker) {
        return;
    }
    fmp4 = zcs_hls_fmp4_create(seg_ms, hls_fmp4_segment_cb, rec);
    if (!fmp4) {
        zcs_http_hls_playlist_destroy(maker);
        return;
    }
    /* 首 tick 可能已经按"codec 未就绪"建了 TS 输出；此时还没切出分片，直接丢弃。 */
    if (rec->zm_pkg) {
        zm_packager_close(rec->zm_pkg);
        rec->zm_pkg = NULL;
    }
    if (rec->ts_mux) {
        if (rec->ts_mux_ops && rec->ts_mux_ops->destroy) {
            rec->ts_mux_ops->destroy(rec->ts_mux);
        }
        rec->ts_mux = NULL;
        rec->ts_mux_ops = NULL;
    }
    rec->sent_video_cfg = 0;
    rec->sent_audio_cfg = 0;
    zcs_http_hls_playlist_destroy(rec->maker);
    rec->maker = maker;
    rec->fmp4_mux = fmp4;
    rec->use_fmp4 = 1;
    rec->fmp4_video_track = -1;
    rec->fmp4_audio_track = -1;
    rec->fmp4_init_done = 0;
    rec->fmp4_seg_origin_dts_ms = 0;
    zn_info("HLS live: fMP4 enabled late for %s/%s (codec ready after recorder start)",
             rec->src->app, rec->src->stream);
}

/*
 * 懒创建 TS 输出。首 tick 才建而不是 create 时建，是因为此刻 track 配置
 * （尤其是音频 codec）可能还没到齐，提前建就选错了封装路径。
 *
 * 两级策略：优先 zm_packager（支持 H.264/H.265 + AAC，内部处理参数集）；
 * 若音频不是 AAC（packager 不支持），回退到 legacy zcs_container TS mux。
 * 回退前把已取的 tracks 全部 clear，避免泄漏。
 */
static void hls_ensure_ts_output(zcs_http_hls_segmenter *rec)
{    zm_track tracks[4];
    size_t tn = 0;

    if (!rec || rec->use_fmp4 || rec->zm_pkg || rec->ts_mux || !rec->src) {
        return;
    }
    if (zcs_zm_tracks_from_source(rec->src, tracks, 4, &tn) != ZM_OK || tn == 0) {
        return; /* 配置尚未到达；下个 tick 再试 */
    }
    {
        size_t i;
        int packager_ok = 1;

        /* TS packager 只处理 H.264/H.265 + AAC；其他音频回退 legacy TS。 */
        for (i = 0; i < tn; ++i) {
            if (tracks[i].kind == ZM_KIND_AUDIO && tracks[i].codec != ZM_CODEC_AAC) {
                packager_ok = 0;
            }
        }
        if (!packager_ok) {
            for (i = 0; i < tn; ++i) {
                zm_track_clear(&tracks[i]);
            }
            goto legacy_ts;
        }
    }
    {
        zm_packager_cfg pcfg;
        zm_packager_cb pcb;
        int64_t seg_ms = (int64_t)(rec->seg_duration_sec * 1000.f);

        if (seg_ms <= 0) {
            seg_ms = 2000;
        }
        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.segment_ms = seg_ms;
        pcfg.live = 1;
        memset(&pcb, 0, sizeof(pcb));
        pcb.on_init = hls_zm_on_init;
        pcb.on_segment = hls_zm_on_segment;
        pcb.on_playlist = hls_zm_on_playlist;
        rec->zm_pkg = zm_hls_packager_open(tracks, tn, &pcfg, &pcb, rec);
        for (size_t i = 0; i < tn; ++i) {
            zm_track_clear(&tracks[i]);
        }
        if (rec->zm_pkg) {
            zn_info("HLS live: zm packager active (%s/%s)", rec->src->app, rec->src->stream);
            return;
        }
    }
legacy_ts:
    {
        zcs_container_mux_opts mcfg;
        int64_t seg_ms = (int64_t)(rec->seg_duration_sec * 1000.f);

        if (seg_ms <= 0) {
            seg_ms = 2000;
        }
        memset(&mcfg, 0, sizeof(mcfg));
        mcfg.id = ZCS_CONTAINER_MPEGTS;
        mcfg.segment_duration_ms = seg_ms;
        mcfg.on_segment = hls_media_segment_cb;
        mcfg.user = rec;
        rec->ts_mux_ops = zcs_container_muxer_find(ZCS_CONTAINER_MPEGTS);
        rec->ts_mux =
            rec->ts_mux_ops && rec->ts_mux_ops->create ? rec->ts_mux_ops->create(&mcfg) : NULL;
    }
}

static void zcs_http_hls_segmenter_tick_nolock(zcs_http_hls_segmenter *rec);

/* 定时器回调：与 HTTP 请求线程共用 rec->mu。所有对 rec 状态的读写（含 tick
 * 内部的 reader 读取、playlist 追加）都在锁内，closing 为 1 表示销毁中，
 * 立即放弃，避免在已释放的 rec 上工作。 */
static void hls_segmenter_tick_cb(void *user)
{
    zcs_http_hls_segmenter *rec = (zcs_http_hls_segmenter *)user;

    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (!rec->closing) {
        zcs_http_hls_segmenter_tick_nolock(rec);
    }
    zb_mutex_unlock(rec->mu);
}

static zcs_http_hls_segmenter *hls_segmenter_create_common(zcs_media_source *src,
                                                           const zcs_http_hls_segmenter_opts *o)
{
    zcs_http_hls_segmenter *rec = (zcs_http_hls_segmenter *)calloc(1, sizeof(*rec));
    if (!rec) {
        return NULL;
    }

    zcs_http_hls_playlist_opts mopts = {
        .segment_duration_sec = o->segment_duration_sec,
        .segment_count = o->segment_count,
    };
    rec->src = src;
    rec->force_fmp4 = o->force_fmp4 ? 1 : 0;
    /* 落盘目录：vhost 开启且非默认时才多插一层 vhost，避免默认部署下路径
     * 多出一个无意义的 "default" 目录。app/stream 已通过 name_ok 校验。 */
    if (o->save_path && o->save_path[0] && src && hls_save_name_ok(src->app) &&
        hls_save_name_ok(src->stream)) {
        int n = (zcs_vhost_enabled() && strcmp(src->vhost, ZCS_DEFAULT_VHOST) != 0)
                    ? snprintf(rec->save_dir, sizeof(rec->save_dir), "%s/%s/%s/%s", o->save_path,
                               src->vhost, src->app, src->stream)
                    : snprintf(rec->save_dir, sizeof(rec->save_dir), "%s/%s/%s", o->save_path,
                               src->app, src->stream);
        if (n > 0 && (size_t)n < sizeof(rec->save_dir) && hls_save_mkdir_p(rec->save_dir)) {
            rec->save_enabled = 1;
            zn_info("HLS save path: %s", rec->save_dir);
        } else {
            zn_warn("HLS save path create failed: %s", o->save_path);
        }
    }
    /* use_fmp4 在创建时就定一次；若此刻 codec 未就绪会先按 TS 建，之后由
     * hls_maybe_upgrade_to_fmp4 在首个 tick 补救。create 阶段不尝试 fmp4 之外的
     * 复杂判断，保持路径简单。 */
    rec->use_fmp4 = rec->force_fmp4 || hls_source_needs_fmp4(src);
    mopts.use_fmp4 = rec->use_fmp4;
    rec->maker = zcs_http_hls_playlist_create(&mopts);
    if (!rec->maker) {
        free(rec);
        return NULL;
    }

    int64_t seg_ms = (int64_t)(o->segment_duration_sec * 1000.f);
    if (seg_ms <= 0) {
        seg_ms = 2000;
    }
    if (rec->use_fmp4) {
        rec->fmp4_mux = zcs_hls_fmp4_create(seg_ms, hls_fmp4_segment_cb, rec);
        if (!rec->fmp4_mux) {
            zcs_http_hls_playlist_destroy(rec->maker);
            free(rec);
            return NULL;
        }
        rec->fmp4_video_track = -1;
        rec->fmp4_audio_track = -1;
    } else {
        /* TS 输出在首个 tick 按实际 track 配置懒创建：优先 ZM packager，
         * 不支持时回退 legacy ts_mux。 */
    }

    rec->enable_audio = o->enable_audio;
    /* 分片时长缺省 2s：HLS 直播的经典折中（首屏延迟 vs 请求频率）。 */
    rec->seg_duration_sec = o->segment_duration_sec > 0.f ? o->segment_duration_sec : 2.f;
    rec->seg_count = o->segment_count;
    rec->mu = zb_mutex_create(0);
    if (!rec->mu) {
        zcs_http_hls_segmenter_destroy(rec);
        return NULL;
    }
    zcs_mux_av_timeline_reset(&rec->mux_av);

    rec->annexb_cap = 256 * 1024;
    rec->annexb = (uint8_t *)malloc(rec->annexb_cap);
    rec->mux_buf_cap = rec->annexb_cap;
    rec->mux_buf = (uint8_t *)malloc(rec->mux_buf_cap);
    /* adts 缓冲按 AAC 一帧（含 ADTS 头）预留 8K，足够覆盖常见音轨帧长。 */
    rec->adts_cap = 8 * 1024;
    rec->adts = (uint8_t *)malloc(rec->adts_cap);
    if (!rec->annexb || !rec->mux_buf || !rec->adts) {
        zcs_http_hls_segmenter_destroy(rec);
        return NULL;
    }
    return rec;
}

zcs_http_hls_segmenter *zcs_http_hls_segmenter_create(zcs_media_source *src,
                                                      const zcs_http_hls_segmenter_opts *opts)
{
    zcs_http_hls_segmenter_opts def;
    zcs_http_hls_segmenter *rec;
    const zcs_http_hls_segmenter_opts *o;

    if (!src || !src->gop_queue) {
        return NULL;
    }
    zcs_http_hls_segmenter_default_opts(&def);
    o = opts ? opts : &def;

    rec = hls_segmenter_create_common(src, o);
    if (!rec) {
        return NULL;
    }
    rec->reader = zcs_gop_reader_attach(src->gop_queue);
    if (!rec->reader) {
        zcs_http_hls_segmenter_destroy(rec);
        return NULL;
    }
    zcs_gop_reader_seek_live_key(rec->reader);
    /* 直接 seek 到 key：从任意位置起读会让首个分片缺少起始 IDR，播放器无法解码首屏。 */
    (void)zcs_media_source_segment_rec_set(src, ZCS_SEGMENT_REC_HLS, rec);
    zn_info("HLS recorder start (%s): %s/%s", rec->use_fmp4 ? "fMP4" : "gop_queue", src->app,
             src->stream);
    return rec;
}

/*
 * 销毁。这里先把要用的子对象指针快照到局部变量、再置 closing 并清空 rec 内
 * 字段，最后在锁外逐个销毁：目的是让可能仍在并发运行的 tick/HTTP 线程看到
 * closing=1 后立即退出，而真正释放 reader/mux 等资源发生在锁外——避免在
 * 持锁时调用会回调进本模块的 destroy（潜在重入/死锁）。
 *
 * 跨线程时序：tick 回调绑定在主 poller 线程，而 destroy 常由推流线程
 * （publish_fini / reregister）调用。`zn_timer_stop` 跨线程只负责取消后续调度，
 * **已经派发**的 tick 仍会在主 poller 上执行；若此时这里 destroy 掉 rec->mu
 * 并 free(rec)，回调就会操作已释放的 rec —— 这正是“清流后随机段错误”的另一种
 * 形态。因此当定时器属于其它正在运行的 poller 时，把真正的清理投递到该 poller
 * 上执行：tick 回调与 async 任务在同一线程串行，天然不会并发。
 */
static void hls_segmenter_destroy_now(zcs_http_hls_segmenter *rec);

static void hls_segmenter_destroy_deferred(void *user)
{
    hls_segmenter_destroy_now((zcs_http_hls_segmenter *)user);
}

void zcs_http_hls_segmenter_destroy(zcs_http_hls_segmenter *rec)
{
    zn_poller *pol;

    if (!rec) {
        return;
    }
    if (rec->timer) {
        zn_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    pol = rec->timer_poller;
    if (pol && zn_poller_is_running(pol) && !zn_poller_is_current_thread(pol)) {
        /* 先把 closing 置位（tick 与 HTTP 路径持 rec->mu 读它），确保在清理任务
         * 到达前不再产出/服务新数据；真正释放仍在目标 poller 上串行完成。 */
        if (rec->mu) {
            zb_mutex_lock(rec->mu);
            rec->closing = 1;
            zb_mutex_unlock(rec->mu);
        }
        if (zn_poller_async(pol, hls_segmenter_destroy_deferred, rec, 0) == ZN_OK) {
            return;
        }
    }
    hls_segmenter_destroy_now(rec);
}

static void hls_segmenter_destroy_now(zcs_http_hls_segmenter *rec)
{
    zcs_media_source *src;
    zcs_http_hls_playlist *maker;
    zcs_gop_reader *reader;
    const zcs_container_muxer_ops *ts_mux_ops;
    void *ts_mux;
    zb_mutex *mu;
    int video_armed;
    uint64_t last_mux_dts_ms;
    zcs_codec_id video_codec;

    zcs_hls_fmp4 *fmp4_mux;
    zm_packager *zm_pkg;
    int use_fmp4;

    if (!rec) {
        return;
    }

    if (rec->timer) {
        zn_timer_stop(rec->timer);
        rec->timer = NULL;
    }

    video_armed = rec->video_armed;
    last_mux_dts_ms = rec->last_mux_dts_ms;
    video_codec = rec->video_codec;
    ts_mux_ops = rec->ts_mux_ops;
    ts_mux = rec->ts_mux;
    fmp4_mux = rec->fmp4_mux;
    zm_pkg = rec->zm_pkg;
    use_fmp4 = rec->use_fmp4;
    reader = rec->reader;
    maker = rec->maker;
    src = rec->src;
    mu = rec->mu;

    if (mu) {
        zb_mutex_lock(mu);
        rec->closing = 1;
        if (src && zcs_media_source_segment_rec_get(src, ZCS_SEGMENT_REC_HLS) == rec) {
            /* 只有 sidecar 里仍是自己时才摘除：延迟销毁期间同名录制器可能已重建，
             * 此时不能把新录制器从 sidecar 里抹掉。 */
            (void)zcs_media_source_segment_rec_set(src, ZCS_SEGMENT_REC_HLS, NULL);
        }
        rec->ts_mux = NULL;
        rec->ts_mux_ops = NULL;
        rec->fmp4_mux = NULL;
        rec->zm_pkg = NULL;
        rec->reader = NULL;
        zb_mutex_unlock(mu);
    } else if (src && zcs_media_source_segment_rec_get(src, ZCS_SEGMENT_REC_HLS) == rec) {
        (void)zcs_media_source_segment_rec_set(src, ZCS_SEGMENT_REC_HLS, NULL);
    }

    if (fmp4_mux) {
        zcs_hls_fmp4_destroy(fmp4_mux);
    }
    if (zm_pkg) {
        (void)zm_packager_flush(zm_pkg, 0);
        zm_packager_close(zm_pkg);
    }
    if (ts_mux && ts_mux_ops) {
        if (video_armed) {
            int psi = hls_video_psi(video_codec);
            if (ts_mux_ops->flush) {
                ts_mux_ops->flush(ts_mux, psi, (int64_t)last_mux_dts_ms);
            }
        }
        if (ts_mux_ops->destroy) {
            ts_mux_ops->destroy(ts_mux);
        }
    }
    if (reader) {
        zcs_gop_reader_detach(reader);
    }
    if (maker) {
        zcs_http_hls_playlist_destroy(maker);
    }
    free(rec->annexb);
    free(rec->mux_buf);
    free(rec->adts);
    zcs_codec_param_sets_clear(&rec->params);
    zb_mutex_destroy(mu);
    free(rec);
}

zcs_http_hls_playlist *zcs_http_hls_segmenter_playlist(zcs_http_hls_segmenter *rec)
{
    if (!rec || rec->closing) {
        return NULL;
    }
    return rec->maker;
}

static zn_poller *hls_segmenter_timer_poller(zn_poller *fallback)
{
    zn_poller *main_pol = zcs_http_hls_main_poller();
    return main_pol ? main_pol : fallback;
}

/* 定时器未挂在目标 poller 上时启动；10ms 周期较密，是因为直播 HLS 需要及时
 * 切片、减小首屏与切片延迟。tick 内部有帧数上限，密集触发不会吃满 CPU。 */
void zcs_http_hls_segmenter_bind_timer(zcs_http_hls_segmenter *rec, zn_poller *poller)
{
    zn_poller *bind_pol;

    if (!rec) {
        return;
    }
    bind_pol = hls_segmenter_timer_poller(poller);
    if (!bind_pol || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (rec->closing) {
        rec->timer_poller = bind_pol;
        zb_mutex_unlock(rec->mu);
        return;
    }
    /* 幂等：定时器已挂在目标 poller 上就不再重启。bind_timer 由 HTTP 请求路径
     * 反复调用，重启会打断 tick 节奏，并且从 HTTP poller 线程跨线程停/起定时器。 */
    if (rec->timer && rec->timer_poller == bind_pol) {
        zb_mutex_unlock(rec->mu);
        return;
    }
    rec->timer_poller = bind_pol;
    if (rec->timer) {
        zn_timer_stop(rec->timer);
        rec->timer = NULL;
    }
    rec->timer = zn_timer_start(bind_pol, 10, 1, hls_segmenter_tick_cb, rec);
    zb_mutex_unlock(rec->mu);
}

/* enter/leave 是给“分片读取”用的引用计数：HTTP 线程在读取 playlist/分片期间
 * 计数 >0，销毁/重切时据此判断还有人在用，避免把正在被读的缓冲释放掉。 */
void zcs_http_hls_segmenter_http_enter(zcs_http_hls_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    rec->http_serve_depth++;
    zb_mutex_unlock(rec->mu);
}

void zcs_http_hls_segmenter_http_leave(zcs_http_hls_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (rec->http_serve_depth > 0) {
        --rec->http_serve_depth;
    }
    zb_mutex_unlock(rec->mu);
}

void zcs_http_hls_segmenter_touch_hls(zcs_http_hls_segmenter *rec)
{
    if (rec) {
        rec->last_hls_req_ms = zn_monotonic_ms();
    }
}

uint32_t hls_mux_dts_ms(zcs_http_hls_segmenter *rec, zcs_track_type track, uint32_t ring_dts_ms)
{
    return zcs_mux_av_timeline_pts(&rec->mux_av, track, ring_dts_ms);
}

static int hls_ensure_mux_buf(zcs_http_hls_segmenter *rec, size_t need)
{
    uint8_t *p;
    size_t cap;

    if (!rec) {
        return 0;
    }
    if (rec->mux_buf && need <= rec->mux_buf_cap) {
        return 1;
    }
    cap = rec->mux_buf_cap ? rec->mux_buf_cap : 256u * 1024u;
    while (cap < need) {
        if (cap > (64u * 1024u * 1024u)) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    p = (uint8_t *)realloc(rec->mux_buf, cap);
    if (!p) {
        return 0;
    }
    rec->mux_buf = p;
    rec->mux_buf_cap = cap;
    return 1;
}

/* zm packager 直接吃 ES，不会像 legacy zcs_mpegts_feed_* 那样补齐参数集。
 * HLS(TS) 的每个 sync 帧都要自带 SPS/PPS（H.265 还要 VPS），否则中途加入的
 * 播放器会报 "non-existing PPS"；这里复用 media/codec 的参数集缓存前置。 */
static void hls_zm_prepend_key_params(zcs_http_hls_segmenter *rec, const zcs_gop_slot *slot,
                                       zm_packet *pkt)
{
    size_t full = 0;
    size_t need;

    if (!rec || !slot || !pkt || !slot->keyframe) {
        return;
    }
    if (slot->codec == ZCS_CODEC_H264 && rec->params.sps && rec->params.pps) {
        need = slot->len + rec->params.sps_len + rec->params.pps_len + 64u;
        if (hls_ensure_mux_buf(rec, need) &&
            zcs_h264_annexb_prepend_sps_pps(rec->params.sps, rec->params.sps_len,
                                            rec->params.pps, rec->params.pps_len, slot->data,
                                            slot->len, rec->mux_buf, rec->mux_buf_cap,
                                            &full) == ZN_OK &&
            full > 0) {
            pkt->mem = zm_mem_borrow(rec->mux_buf, full);
        }
    } else if (slot->codec == ZCS_CODEC_H265 && rec->params.sps && rec->params.pps) {
        need = slot->len + rec->params.vps_len + rec->params.sps_len + rec->params.pps_len + 64u;
        if (hls_ensure_mux_buf(rec, need) &&
            zcs_h265_annexb_build_rtp_au(rec->params.vps, rec->params.vps_len, rec->params.sps,
                                         rec->params.sps_len, rec->params.pps, rec->params.pps_len,
                                         slot->data, slot->len, 1, rec->mux_buf, rec->mux_buf_cap,
                                         &full) == ZN_OK &&
            full > 0) {
            pkt->mem = zm_mem_borrow(rec->mux_buf, full);
        }
    }
}

/*
 * 单次 tick 的实际工作（调用方须已持 rec->mu）。按输出后端三选一：
 *   1. fMP4：先把音轨补齐（AAC/Opus 配置到达后才能建 init），再按 codec 分流；
 *   2. zm_packager(TS)：吃 ES，sync 帧前手动补 SPS/PPS；
 *   3. legacy TS mux：各自 feed_*，首次还要先送 config。
 * 三者都用 read_muxed 读交织后的帧，且都跳过 config_frame（配置单独走
 * sent_*_cfg 通道），避免把参数集当成普通媒体帧重复写入。
 * 每 tick 最多读 ZCS_SEGMENT_REC_TICK_FRAMES 帧，防止一次 tick 独占过久。
 */
static void zcs_http_hls_segmenter_tick_nolock(zcs_http_hls_segmenter *rec)
{
    zcs_gop_slot slot;

    if (!rec || rec->closing || !rec->src) {
        return;
    }
    if (!rec->reader || !rec->src->gop_queue) {
        return;
    }
    hls_maybe_upgrade_to_fmp4(rec);
    hls_ensure_ts_output(rec);

    if (rec->use_fmp4) {
        if (!rec->sent_audio_cfg && rec->enable_audio && rec->src) {
            size_t clen = 0;
            const uint8_t *cfg = zcs_gop_queue_audio_config(rec->src->gop_queue, &clen);
            if (cfg && clen) {
                hls_fmp4_ensure_aac_track(rec, cfg, clen);
            } else if (rec->src->audio.ready && rec->src->audio.codec == ZCS_CODEC_OPUS) {
                hls_fmp4_ensure_opus_track(rec);
            }
        }
        for (int n = 0; n < ZCS_SEGMENT_REC_TICK_FRAMES &&
                        zcs_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
             ++n) {
            if (!slot.data || slot.len == 0 || slot.config_frame) {
                continue;
            }
            if (slot.track == ZCS_TRACK_VIDEO) {
                zcs_gop_slot_refresh_play_key(&slot);
                if (slot.codec == ZCS_CODEC_VP8 || slot.codec == ZCS_CODEC_VP9) {
                    hls_fmp4_feed_vpx(rec, slot.codec, slot.data, slot.len, slot.dts_ms,
                                      slot.keyframe);
                } else if (slot.codec == ZCS_CODEC_AV1) {
                    hls_fmp4_feed_av1(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
                } else if (slot.codec == ZCS_CODEC_H264 || slot.codec == ZCS_CODEC_H265) {
                    hls_fmp4_feed_h26x(rec, slot.codec, slot.data, slot.len, slot.dts_ms,
                                       slot.keyframe);
                }
            } else if (slot.track == ZCS_TRACK_AUDIO) {
                if (slot.codec == ZCS_CODEC_OPUS) {
                    hls_fmp4_feed_opus(rec, slot.data, slot.len, slot.dts_ms);
                } else if (slot.codec == ZCS_CODEC_AAC) {
                    hls_fmp4_feed_aac(rec, slot.data, slot.len, slot.dts_ms);
                }
            }
        }
        return;
    }

    if (rec->zm_pkg) {
        if ((!rec->params.sps || !rec->params.pps) && rec->src && rec->src->gop_queue) {
            size_t clen = 0;
            const uint8_t *cfg = zcs_gop_queue_video_config(rec->src->gop_queue, &clen);
            if (cfg && clen) {
                (void)zcs_codec_param_sets_cache_flv_cfg(&rec->params, cfg, clen);
            }
        }
        for (int n = 0;
             n < ZCS_SEGMENT_REC_TICK_FRAMES && zcs_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
             ++n) {
            zm_packet pkt;
            if (!slot.data || slot.len == 0 || slot.config_frame) {
                continue;
            }
            if (slot.track == ZCS_TRACK_VIDEO) {
                zcs_gop_slot_refresh_play_key(&slot);
            }
            zcs_zm_packet_from_gop_slot(&slot, &pkt);
            pkt.dts_ms = (int64_t)hls_mux_dts_ms(rec, slot.track, slot.dts_ms);
            pkt.pts_ms = pkt.dts_ms;
            hls_zm_prepend_key_params(rec, &slot, &pkt);
            (void)zm_packager_write(rec->zm_pkg, &pkt);
        }
        return;
    }

    if (!rec->sent_video_cfg) {
        size_t clen = 0;
        const uint8_t *cfg = zcs_gop_queue_video_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            feed_video_tag_cfg(rec, cfg, clen);
            rec->sent_video_cfg = 1;
        }
    }
    if (!rec->sent_audio_cfg && rec->enable_audio) {
        size_t clen = 0;
        const uint8_t *cfg = zcs_gop_queue_audio_config(rec->src->gop_queue, &clen);
        if (cfg && clen) {
            feed_audio_tag_cfg(rec, cfg, clen);
            if (!rec->sent_audio_cfg) {
                rec->sent_audio_cfg = 1;
            }
        } else if (rec->src && rec->src->audio.ready && rec->src->audio.codec == ZCS_CODEC_OPUS) {
            hls_ensure_opus_extradata(rec);
        }
    }

    for (int n = 0;
         n < ZCS_SEGMENT_REC_TICK_FRAMES && zcs_gop_reader_read_muxed(rec->reader, &slot, 0) > 0;
         ++n) {
        if (!slot.data || slot.len == 0 || slot.config_frame) {
            continue;
        }
        if (slot.track == ZCS_TRACK_VIDEO) {
            zcs_gop_slot_refresh_play_key(&slot);
            if (slot.codec == ZCS_CODEC_H265) {
                feed_h265_video_es(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_H264) {
                feed_h264_video_es(rec, slot.data, slot.len, slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_H266) {
                feed_raw_video_es(rec, hls_video_psi(ZCS_CODEC_H266), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_AV1) {
                feed_raw_video_es(rec, hls_video_psi(ZCS_CODEC_AV1), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_VP8) {
                feed_raw_video_es(rec, hls_video_psi(ZCS_CODEC_VP8), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            } else if (slot.codec == ZCS_CODEC_VP9) {
                feed_raw_video_es(rec, hls_video_psi(ZCS_CODEC_VP9), slot.data, slot.len,
                                  slot.dts_ms, slot.keyframe);
            }
        } else if (slot.track == ZCS_TRACK_AUDIO) {
            feed_audio_es(rec, slot.data, slot.len, slot.dts_ms, slot.codec);
        }
    }
}

void zcs_http_hls_segmenter_lock(zcs_http_hls_segmenter *rec)
{
    if (rec && rec->mu) {
        zb_mutex_lock(rec->mu);
    }
}

void zcs_http_hls_segmenter_unlock(zcs_http_hls_segmenter *rec)
{
    if (rec && rec->mu) {
        zb_mutex_unlock(rec->mu);
    }
}

void zcs_http_hls_segmenter_tick_locked(zcs_http_hls_segmenter *rec)
{
    zcs_http_hls_segmenter_tick_nolock(rec);
}

/* 对外 tick 入口：加锁后跑 tick_nolock。定时器回调与手动 tick 都走这里，
 * closing 时跳过（销毁中）。 */
void zcs_http_hls_segmenter_tick(zcs_http_hls_segmenter *rec)
{
    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (!rec->closing) {
        zcs_http_hls_segmenter_tick_nolock(rec);
    }
    zb_mutex_unlock(rec->mu);
}

/**
 * 仅在尚无分片时于 m3u8 请求路径上 pump（与 DASH MPD 策略一致）。
 * 已有分片后依赖 10ms timer；每次 m3u8 狂抽 gop_reader 会抽空交织窗口，导致切段停滞。
 */
static void hls_segmenter_serve_pump_m3u8(zcs_http_hls_segmenter *rec)
{
    int i;

    if (!rec || !rec->mu) {
        return;
    }
    zb_mutex_lock(rec->mu);
    if (!rec->closing) {
        for (i = 0; i < ZCS_SEGMENT_REC_SERVE_MAX_TICKS; ++i) {
            zcs_http_hls_segmenter_tick_nolock(rec);
        }
        if (rec->use_fmp4) {
            hls_fmp4_serve_flush(rec);
        }
    }
    zb_mutex_unlock(rec->mu);
}

zn_err_t zcs_http_hls_segmenter_serve_m3u8(zcs_http_hls_segmenter *rec, char *out, size_t cap,
                                            size_t *out_len, int max_ticks, int *seg_count)
{
    zcs_http_hls_playlist *maker;
    zn_err_t err;

    (void)max_ticks;
    if (seg_count) {
        *seg_count = 0;
    }
    if (!rec || !rec->mu || !out || !out_len) {
        return ZN_ERR_INVALID;
    }

    maker = rec->maker;
    if (!maker) {
        return ZN_ERR_INVALID;
    }

    /* 已有分片时勿在每次 m3u8 轮询时 pump：高频刷新会抽空 reader，切段停滞（同 DASH）。 */
    if (zcs_http_hls_playlist_segment_count(maker) <= 0) {
        hls_segmenter_serve_pump_m3u8(rec);
    }

    zb_mutex_lock(rec->mu);
    if (rec->closing) {
        zb_mutex_unlock(rec->mu);
        return ZN_ERR_INVALID;
    }
    err = zcs_http_hls_playlist_build_m3u8(maker, out, cap, out_len);
    if (err == ZN_OK && seg_count) {
        *seg_count = zcs_http_hls_playlist_segment_count(maker);
    }
    if (err == ZN_OK) {
        zcs_http_hls_segmenter_touch_hls(rec);
    }
    zb_mutex_unlock(rec->mu);
    return err;
}

zn_err_t zcs_http_hls_segmenter_copy_segment(zcs_http_hls_segmenter *rec, const char *name,
                                              uint8_t *buf, size_t cap, size_t *out_len,
                                              int max_ticks)
{
    zcs_http_hls_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZCS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    maker = rec->maker;
    if (!maker) {
        return ZN_ERR_INVALID;
    }

    if (zcs_http_hls_playlist_copy_segment(maker, name, buf, cap, out_len) == ZN_OK &&
        *out_len > 0) {
        zcs_http_hls_segmenter_touch_hls(rec);
        return ZN_OK;
    }

    /* 直接取不到（分片可能还没切出来）就主动 tick 若干次催它产生，再重试。
     * 这是 HTTP 请求路径上的“拉一把”，避免客户端首次请求分片时因定时器
     * 还没跑到而 404。每次 tick 单独加锁、放锁，给其它线程留出执行机会。 */
    for (i = 0; i < max_ticks; ++i) {
        zb_mutex_lock(rec->mu);
        if (rec->closing) {
            zb_mutex_unlock(rec->mu);
            return ZN_ERR_INVALID;
        }
        zcs_http_hls_segmenter_tick_nolock(rec);
        zb_mutex_unlock(rec->mu);
        if (zcs_http_hls_playlist_copy_segment(maker, name, buf, cap, out_len) == ZN_OK &&
            *out_len > 0) {
            zcs_http_hls_segmenter_touch_hls(rec);
            return ZN_OK;
        }
    }
    *out_len = 0;
    return ZN_ERR_INVALID;
}

/* 引用式取分片（不拷贝，返回 buf 引用）。与 copy_segment 同样“取不到就 tick
 * 催产”，区别是不复制正文，适合内部零拷贝发送路径。 */
zn_err_t zcs_http_hls_segmenter_ref_segment(zcs_http_hls_segmenter *rec, const char *name,
                                             zn_buf **out_buf, size_t *out_len, int max_ticks)
{
    zcs_http_hls_playlist *maker;
    int i;

    if (!rec || !rec->mu || !name || !out_buf || !out_len) {
        return ZN_ERR_INVALID;
    }
    if (max_ticks <= 0) {
        max_ticks = ZCS_SEGMENT_REC_SERVE_MAX_TICKS;
    }

    maker = rec->maker;
    if (!maker) {
        return ZN_ERR_INVALID;
    }

    if (zcs_http_hls_playlist_ref_segment(maker, name, out_buf, out_len) == ZN_OK &&
        *out_len > 0) {
        zcs_http_hls_segmenter_touch_hls(rec);
        return ZN_OK;
    }

    for (i = 0; i < max_ticks; ++i) {
        zb_mutex_lock(rec->mu);
        if (rec->closing) {
            zb_mutex_unlock(rec->mu);
            return ZN_ERR_INVALID;
        }
        zcs_http_hls_segmenter_tick_nolock(rec);
        zb_mutex_unlock(rec->mu);
        if (zcs_http_hls_playlist_ref_segment(maker, name, out_buf, out_len) == ZN_OK &&
            *out_len > 0) {
            zcs_http_hls_segmenter_touch_hls(rec);
            return ZN_OK;
        }
    }
    *out_len = 0;
    return ZN_ERR_INVALID;
}

static zn_err_t hls_ops_create_live(zcs_media_source *src, const void *opts, void **out_rec)
{
    zcs_http_hls_segmenter *rec;

    if (!out_rec) {
        return ZN_ERR_INVALID;
    }
    rec = zcs_http_hls_segmenter_create(src, (const zcs_http_hls_segmenter_opts *)opts);
    if (!rec) {
        return ZN_ERR_INVALID;
    }
    *out_rec = rec;
    return ZN_OK;
}

static void hls_ops_destroy(void *rec)
{
    zcs_http_hls_segmenter_destroy((zcs_http_hls_segmenter *)rec);
}

static void hls_ops_bind_timer(void *rec, zn_poller *poller)
{
    zcs_http_hls_segmenter_bind_timer((zcs_http_hls_segmenter *)rec, poller);
}

static void hls_ops_tick(void *rec)
{
    zcs_http_hls_segmenter_tick((zcs_http_hls_segmenter *)rec);
}

static void hls_ops_touch(void *rec)
{
    zcs_http_hls_segmenter_touch_hls((zcs_http_hls_segmenter *)rec);
}

static const zcs_segment_recorder_ops k_hls_ops = {
    .name = ZCS_SEGMENT_REC_HLS,
    .create_live = hls_ops_create_live,
    .destroy = hls_ops_destroy,
    .bind_timer = hls_ops_bind_timer,
    .tick = hls_ops_tick,
    .touch = hls_ops_touch,
};

const zcs_segment_recorder_ops *zcs_http_hls_segment_recorder_ops(void)
{
    return &k_hls_ops;
}
