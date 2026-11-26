/**
 * @file egress_zs_recorder.c
 * @brief 云存储录制器实现：GOP 队列 -> libmpeg PS -> zs_writer -> 预分配槽位。
 */
#include "zms/egress/recorder/egress_zs_recorder.h"

#include "bridge/zm_ps_muxer.h"
#include "storage/zs_clock.h"
#include "storage/zs_config.h"
#include "storage/zs_pool.h"
#include "storage/zs_writer.h"
#include "zms/core/event.h"
#include "zms/core/gop/queue.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zms/media/codec/param_sets.h"
#include "zms/media/clock.h"
#include "zb/os/sync.h"
#include "zn/platform.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZS_REC_TICK_MS 20u
/* 每 tick 最多消费帧数：既跟得上码率，又不长时间占用 poller。 */
#define ZS_REC_TICK_FRAMES 64u
#define ZS_PTS_MASK ((INT64_C(1) << 33) - 1)

typedef struct zs_recorder {
    zms_media_source *src;
    zms_gop_reader *reader;
    zn_timer *timer;
    zn_poller *poller;
    zb_mutex *mu;
    int closing;

    zs_writer *writer;
    zs_clock *clock;
    zms_zm_ps_muxer *ps_mux;
    int video_stream;
    int audio_stream;

    zms_codec_param_sets params;
    uint8_t *au;
    size_t au_cap;
    char stream[ZS_SLOT_STREAM_MAX];
} zs_recorder;

static struct {
    zms_storage_config cfg;
    zs_pool *pool;
    zb_mutex *mu; /* 保护 pool 懒创建 */
    int enabled;
    uint64_t last_gc_ms; /* 上次保留期回收时刻 */
} g_zs;

/* ── 参数集 ─────────────────────────────────────────────────────────────── */

static void rec_params_store(zs_recorder *r, const uint8_t *vps, size_t vps_len, const uint8_t *sps,
                             size_t sps_len, const uint8_t *pps, size_t pps_len)
{
    zms_codec_param_sets_clear(&r->params);
    if (vps && vps_len) {
        r->params.vps = (uint8_t *)malloc(vps_len);
        if (r->params.vps) {
            memcpy(r->params.vps, vps, vps_len);
            r->params.vps_len = vps_len;
        }
    }
    if (sps && sps_len) {
        r->params.sps = (uint8_t *)malloc(sps_len);
        if (r->params.sps) {
            memcpy(r->params.sps, sps, sps_len);
            r->params.sps_len = sps_len;
        }
    }
    if (pps && pps_len) {
        r->params.pps = (uint8_t *)malloc(pps_len);
        if (r->params.pps) {
            memcpy(r->params.pps, pps, pps_len);
            r->params.pps_len = pps_len;
        }
    }
}

static void rec_refresh_params(zs_recorder *r, const zms_gop_slot *slot)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    const uint8_t *vps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;
    size_t vps_len = 0;

    if (slot->codec == ZMS_CODEC_H264) {
        if (zms_h264_annexb_extract_sps_pps(slot->data, slot->len, &sps, &sps_len, &pps,
                                            &pps_len)) {
            rec_params_store(r, NULL, 0, sps, sps_len, pps, pps_len);
        }
    } else if (slot->codec == ZMS_CODEC_H265) {
        if (zms_h265_annexb_extract_vps_sps_pps(slot->data, slot->len, &vps, &vps_len, &sps,
                                                &sps_len, &pps, &pps_len)) {
            rec_params_store(r, vps, vps_len, sps, sps_len, pps, pps_len);
        }
    }
}

static int rec_reserve_au(zs_recorder *r, size_t need)
{
    uint8_t *p;

    if (r->au_cap >= need) {
        return 0;
    }
    p = (uint8_t *)realloc(r->au, need);
    if (!p) {
        return -1;
    }
    r->au = p;
    r->au_cap = need;
    return 0;
}

/**
 * 关键帧前补齐参数集：GB28181/PS 下游要能随时从 GOP 头解起。
 * 非关键帧直接返回原始 ES（不引入额外拷贝）。
 */
static const uint8_t *rec_prepare_video(zs_recorder *r, const zms_gop_slot *slot, size_t *out_len)
{
    size_t need;

    *out_len = slot->len;
    if (!slot->keyframe || !slot->data || slot->len == 0) {
        return slot->data;
    }
    rec_refresh_params(r, slot);
    if (slot->codec == ZMS_CODEC_H264) {
        size_t n = 0;

        if (!r->params.sps || !r->params.pps) {
            return slot->data;
        }
        need = slot->len + r->params.sps_len + r->params.pps_len + 64;
        if (rec_reserve_au(r, need) != 0) {
            return slot->data;
        }
        if (zms_h264_annexb_prepend_sps_pps(r->params.sps, r->params.sps_len, r->params.pps,
                                            r->params.pps_len, slot->data, slot->len, r->au,
                                            r->au_cap, &n) == ZN_OK &&
            n > 0) {
            *out_len = n;
            return r->au;
        }
        return slot->data;
    }
    if (slot->codec == ZMS_CODEC_H265) {
        size_t n = 0;
        int have = r->params.vps && r->params.vps_len && r->params.sps && r->params.sps_len &&
                   r->params.pps && r->params.pps_len;

        need = slot->len + r->params.vps_len + r->params.sps_len + r->params.pps_len + 64;
        if (rec_reserve_au(r, need) != 0) {
            return slot->data;
        }
        if (zms_h265_annexb_build_rtp_au(r->params.vps, r->params.vps_len, r->params.sps,
                                         r->params.sps_len, r->params.pps, r->params.pps_len,
                                         slot->data, slot->len, have, r->au, r->au_cap, &n) ==
                ZN_OK &&
            n > 0) {
            *out_len = n;
            return r->au;
        }
        return slot->data;
    }
    return slot->data;
}

/* ── PS 写回调 -> 槽位写入器 ─────────────────────────────────────────────── */

static int rec_ps_write(void *user, int stream_id, const uint8_t *ps, size_t len)
{
    zs_recorder *r = (zs_recorder *)user;

    (void)stream_id;
    if (!r || !r->writer || !ps || len == 0) {
        return -1;
    }
    if (zs_writer_push(r->writer, ps, len, zn_monotonic_ms()) != 0) {
        return -1;
    }
    return (int)len;
}

/* ── 喂帧 ───────────────────────────────────────────────────────────────── */

/* PS 复用器按实际到达的码流创建：推流刚开始时源还没有视频信息。 */
static int rec_ensure_muxer(zs_recorder *r, zms_codec_id codec)
{
    if (r->ps_mux) {
        return 0;
    }
    r->ps_mux = zms_zm_ps_muxer_create(&(zms_zm_ps_muxer_opts){rec_ps_write, r});
    if (!r->ps_mux) {
        return -1;
    }
    r->video_stream = zms_zm_ps_muxer_add_video(r->ps_mux, codec);
    if (r->video_stream <= 0) {
        zms_zm_ps_muxer_destroy(r->ps_mux);
        r->ps_mux = NULL;
        return -1;
    }
    if (r->src && r->src->has_audio) {
        r->audio_stream = zms_zm_ps_muxer_add_audio(r->ps_mux, r->src->audio.codec);
    }
    return 0;
}

static void rec_feed(zs_recorder *r, const zms_gop_slot *slot)
{
    zs_clock_out co;
    uint32_t dts;
    uint32_t pts;
    int64_t dts90;
    int64_t pts90;
    int key;

    if (!slot->data || slot->len == 0) {
        return;
    }
    if (slot->config_frame) {
        /* 参数集单独一帧：只在关键帧处补齐，不单独落盘 */
        if (slot->track == ZMS_TRACK_VIDEO) {
            rec_refresh_params(r, slot);
        }
        return;
    }

    dts = slot->dts_ms;
    pts = slot->pts_ms ? slot->pts_ms : dts;
    zs_clock_input(r->clock, (int64_t)dts * 90, (int64_t)zms_wall_ms(), 0, &co);
    if (!co.valid) {
        return;
    }
    /* 存储索引存 64 位归一化时间；写进 PES 的必须是 33 位（PS 规范） */
    dts90 = co.pts_90k;
    pts90 = dts90 + (int64_t)(pts - dts) * 90;
    if (pts90 < dts90) {
        pts90 = dts90;
    }

    if (slot->track == ZMS_TRACK_VIDEO) {
        size_t au_len = 0;
        const uint8_t *au;

        if (rec_ensure_muxer(r, slot->codec) != 0) {
            return;
        }
        au = rec_prepare_video(r, slot, &au_len);
        if (!au || au_len == 0) {
            return;
        }
        key = slot->keyframe;
        zs_writer_begin_frame(r->writer, key, co.pts_90k, co.utc_ms, zn_monotonic_ms());
        (void)zms_zm_ps_muxer_input_video(r->ps_mux, r->video_stream, au, au_len, key,
                                          pts90 & ZS_PTS_MASK, dts90 & ZS_PTS_MASK);
    } else if (slot->track == ZMS_TRACK_AUDIO) {
        if (!r->ps_mux || r->audio_stream <= 0) {
            return; /* 视频还没到，PS 复用器未建立；丢弃早期音频 */
        }
        (void)zms_zm_ps_muxer_input_audio(r->ps_mux, r->audio_stream, slot->data, slot->len,
                                          pts90 & ZS_PTS_MASK);
    }
}

/* 保留期回收：每分钟最多一次（跨通道共用，放在录制 tick 里顺带做）。 */
static void zs_gc_tick(void)
{
    uint64_t now = zn_monotonic_ms();

    if (g_zs.last_gc_ms != 0 && now < g_zs.last_gc_ms + 60000u) {
        return;
    }
    g_zs.last_gc_ms = now;
    if (g_zs.pool) {
        uint32_t n = zs_pool_expire(g_zs.pool, zms_wall_ms());

        if (n > 0) {
            zn_info("[zs] 已回收过期槽位 %u 个", n);
        }
    }
}

static void rec_pump(zs_recorder *r)
{
    zms_gop_slot slot;
    unsigned n;

    if (!r->reader || !r->writer) {
        return;
    }
    for (n = 0; n < ZS_REC_TICK_FRAMES && zms_gop_reader_read_muxed(r->reader, &slot, 0) > 0; ++n) {
        rec_feed(r, &slot);
    }
    (void)zs_writer_tick(r->writer, zn_monotonic_ms());
    zs_gc_tick();
}

static void rec_tick_cb(void *user)
{
    zs_recorder *r = (zs_recorder *)user;

    if (!r || !r->mu) {
        return;
    }
    zb_mutex_lock(r->mu);
    if (!r->closing) {
        rec_pump(r);
    }
    zb_mutex_unlock(r->mu);
}

/* ── 槽位池 ─────────────────────────────────────────────────────────────── */

static zs_pool *zs_pool_get(void)
{
    zs_pool *p;

    if (!g_zs.mu) {
        return NULL;
    }
    zb_mutex_lock(g_zs.mu);
    if (!g_zs.pool && g_zs.cfg.dir[0]) {
        zs_pool_opts o;

        zs_pool_opts_from_config(&o, &g_zs.cfg);
        g_zs.pool = zs_pool_open(&o);
        if (!g_zs.pool) {
            zn_error("[zs] 槽位池打开失败 dir=%s", g_zs.cfg.dir);
        } else {
            zn_info("[zs] 槽位池就绪 dir=%s capacity=%lluGB slots=%u retention=%ud",
                    g_zs.cfg.dir, (unsigned long long)g_zs.cfg.capacity_gb,
                    zs_pool_max_slots(g_zs.pool), g_zs.cfg.retention_days);
        }
    }
    p = g_zs.pool;
    zb_mutex_unlock(g_zs.mu);
    return p;
}

/* ── 生命周期 ───────────────────────────────────────────────────────────── */

zn_err_t zms_zs_recorder_start(zms_media_source *src)
{
    zs_recorder *r;
    zs_pool *pool;
    zs_writer_opts wo;
    zn_poller *pol;

    if (!src || !src->gop_queue || !g_zs.enabled) {
        return ZN_ERR_INVALID;
    }
    if (src->zs_recorder) {
        return ZN_OK;
    }
    pool = zs_pool_get();
    if (!pool) {
        return ZN_ERR_STATE;
    }
    pol = zms_media_events_poller();
    if (!pol) {
        return ZN_ERR_STATE;
    }

    r = (zs_recorder *)calloc(1, sizeof(*r));
    if (!r) {
        return ZN_ERR_NOMEM;
    }
    r->src = src;
    r->poller = pol;
    r->mu = zb_mutex_create(0);
    r->clock = zs_clock_create(NULL);
    zms_codec_param_sets_init(&r->params);
    snprintf(r->stream, sizeof(r->stream), "%s/%s", src->app[0] ? src->app : "live", src->stream);

    if (!r->mu || !r->clock) {
        goto fail;
    }

    memset(&wo, 0, sizeof(wo));
    wo.pool = pool;
    wo.channel_id = 0;
    wo.stream = r->stream;
    zs_writer_opts_from_config(&wo, &g_zs.cfg, pool, 0, r->stream);
    r->writer = zs_writer_create(&wo);
    if (!r->writer) {
        goto fail;
    }

    r->reader = zms_gop_reader_attach(src->gop_queue);
    if (!r->reader) {
        goto fail;
    }
    zms_gop_reader_seek_live_key(r->reader);

    src->zs_recorder = r;
    src->zs_recorder_stop = zms_zs_recorder_stop;
    r->timer = zn_timer_start(pol, ZS_REC_TICK_MS, 1, rec_tick_cb, r);
    if (!r->timer) {
        src->zs_recorder = NULL;
        src->zs_recorder_stop = NULL;
        goto fail;
    }
    zn_info("[zs] 云存储录制已启动 %s", r->stream);
    return ZN_OK;

fail:
    if (r->reader) {
        zms_gop_reader_detach(r->reader);
    }
    if (r->writer) {
        zs_writer_destroy(r->writer);
    }
    if (r->ps_mux) {
        zms_zm_ps_muxer_destroy(r->ps_mux);
    }
    if (r->clock) {
        zs_clock_destroy(r->clock);
    }
    zms_codec_param_sets_clear(&r->params);
    if (r->mu) {
        zb_mutex_destroy(r->mu);
    }
    free(r->au);
    free(r);
    return ZN_ERR_STATE;
}

void zms_zs_recorder_stop(zms_media_source *src)
{
    zs_recorder *r;
    zms_media_source *s;

    if (!src) {
        return;
    }
    r = (zs_recorder *)src->zs_recorder;
    if (!r) {
        return;
    }
    s = r->src;
    if (r->timer) {
        zn_timer_stop(r->timer);
        r->timer = NULL;
    }
    if (r->mu) {
        zb_mutex_lock(r->mu);
        r->closing = 1;
        if (r->reader) {
            zms_gop_reader_detach(r->reader);
            r->reader = NULL;
        }
        if (s && s->zs_recorder == r) {
            s->zs_recorder = NULL;
            s->zs_recorder_stop = NULL;
        }
        zb_mutex_unlock(r->mu);
        zb_mutex_destroy(r->mu);
        r->mu = NULL;
    }
    /* 收尾：刷盘 + 封口。放在锁外，避免长时间占用录制线程。 */
    if (r->writer) {
        zs_writer_close(r->writer);
        zs_writer_destroy(r->writer);
        r->writer = NULL;
    }
    if (r->ps_mux) {
        zms_zm_ps_muxer_destroy(r->ps_mux);
        r->ps_mux = NULL;
    }
    if (r->clock) {
        zs_clock_destroy(r->clock);
        r->clock = NULL;
    }
    zms_codec_param_sets_clear(&r->params);
    free(r->au);
    zn_info("[zs] 云存储录制已停止");
    free(r);
}

/* ── 配置与查询 ─────────────────────────────────────────────────────────── */

static void zs_autostart(zms_media_source *src)
{
    (void)zms_zs_recorder_start(src);
}

void zms_zs_recorder_configure(const zms_storage_config *cfg)
{
    if (!cfg) {
        return;
    }
    g_zs.cfg = *cfg;
    if (!g_zs.mu) {
        g_zs.mu = zb_mutex_create(0);
    }
    if (g_zs.enabled && !cfg->enable) {
        g_zs.enabled = 0;
        zms_media_events_set_recorder_autostart(NULL);
        return;
    }
    g_zs.enabled = cfg->enable ? 1 : 0;
    if (g_zs.enabled) {
        zms_media_events_set_recorder_autostart(zs_autostart);
        zn_info("[zs] 云存储已启用 dir=%s capacity=%dGB retention=%dd", cfg->dir, cfg->capacity_gb,
                cfg->retention_days);
        /* 启动即打开池：扫描目录、恢复上次未封口的槽位、按保留期回收，
         * 不必等到第一路推流才开始自愈。 */
        (void)zs_pool_get();
    } else {
        zms_media_events_set_recorder_autostart(NULL);
    }
}

int zms_zs_recorder_enabled(void)
{
    return g_zs.enabled;
}

void zms_zs_recorder_stats(uint32_t *slots, uint32_t *free_slots, uint64_t *used_bytes,
                           uint64_t *capacity)
{
    zs_pool *p = g_zs.pool;

    if (slots) {
        *slots = p ? zs_pool_count(p) : 0;
    }
    if (free_slots) {
        *free_slots = p ? zs_pool_free_count(p) : 0;
    }
    if (used_bytes) {
        *used_bytes = p ? zs_pool_used_bytes(p) : 0;
    }
    if (capacity) {
        *capacity = p ? zs_pool_capacity_bytes(p) : 0;
    }
}
