/**
 * @file hls.c
 * @brief VOD HLS 清单/分片解析与静态打包。
 */
#include "zcs/vod/play/hls.h"
#include "zcs/vod/io/source.h"
#include "zcs/vod/play/play_lane.h"
#include "zcs/media/container/flv/flv_index.h"
#include "zcs/media/codec/param_sets.h"
#include "zcs/core/gop/queue.h"
#include "zcs/media/codec/h264/h264_over_rtmp.h"
#include "zcs/media/container/flv/flv_tag_probe.h"
#include "zcs/media/codec/aac/aac_over_rtmp.h"
#include "zcs/media/codec/h264/h264_es.h"
#include "zcs/media/codec/h265/h265_es.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/media/container/container_registry.h"
#include "zcs/media/codec/aac/aac_config.h"
#include "mpeg4-hevc.h"
#include "zn/util/log.h"
#include "zb/os/sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#define zcs_vod_mkdir(path) _mkdir(path)
#define zcs_vod_is_dir(st) (((st).st_mode & _S_IFDIR) != 0)
#else
#include <dirent.h>
#include <sys/stat.h>
#define zcs_vod_mkdir(path) mkdir(path, 0755)
#define zcs_vod_is_dir(st) S_ISDIR((st).st_mode)
#endif

/** m3u8/TS 格式代际：变更 mux 时间戳策略时递增，并清旧 .ts 缓存 */
#define ZCS_VOD_HLS_GEN 5
#define ZCS_VOD_HLS_GEN_LINE "# ZCS-HLS-GEN:5\r\n"

static zb_mutex *g_vod_hls_mu;

static int file_mtime(const char *path, time_t *out)
{
    struct stat st;
    if (!path || !out || stat(path, &st) != 0) {
        return 0;
    }
    *out = st.st_mtime;
    return 1;
}

static int vod_hls_media_dir(const char *app, const char *stream, char *out, size_t out_cap)
{
    const zcs_vod_config *cfg = zcs_vod_config_get();
    char rel[ZCS_STREAM_MAX];
    char *slash;
    size_t n;

    if (!app || !stream || !out || out_cap == 0) {
        return 0;
    }
    strncpy(rel, stream, sizeof(rel) - 1);
    rel[sizeof(rel) - 1] = '\0';
    slash = strrchr(rel, '/');
    if (slash) {
        *slash = '\0';
    } else {
        rel[0] = '\0'; /* root-level file: media dir is {root}/{app} */
    }
    if (rel[0]) {
        n = (size_t)snprintf(out, out_cap, "%s/%s/%s", cfg->record_root, app, rel);
    } else {
        n = (size_t)snprintf(out, out_cap, "%s/%s", cfg->record_root, app);
    }
    return n > 0 && n < out_cap;
}

static int playlist_file_valid(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size > 16;
}

static int segment_file_valid(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size >= 188;
}

static int rel_path_is_ts(const char *rel)
{
    size_t n;

    if (!rel || !zcs_vod_rel_path_safe(rel)) {
        return 0;
    }
    n = strlen(rel);
    return n >= 4 && strcmp(rel + n - 3, ".ts") == 0;
}

/* 把 stream 名换成 .m3u8 相对路径并解析到磁盘，最后校验文件有效（>16 字节，
 * 排除空壳）。用于判断“是否已有可直接使用的清单”。 */
int zcs_vod_hls_resolve_playlist(const char *app, const char *stream, char *out, size_t out_cap)
{
    char m3u8_rel[ZCS_STREAM_MAX];

    if (!app || !stream || !out || out_cap == 0) {
        return 0;
    }
    if (!zcs_vod_mp4_stream_to_m3u8_rel(stream, m3u8_rel, sizeof(m3u8_rel))) {
        return 0;
    }
    if (!zcs_vod_resolve_rel_path(app, m3u8_rel, out, out_cap)) {
        return 0;
    }
    return playlist_file_valid(out);
}

int zcs_vod_hls_has_static_pack(const char *app, const char *stream)
{
    char path[512];
    return zcs_vod_hls_resolve_playlist(app, stream, path, sizeof(path));
}

int zcs_vod_hls_playlist_gen(const char *m3u8_path)
{
    char buf[512];
    const char *p;
    FILE *fp;
    size_t n;

    if (!m3u8_path || !m3u8_path[0]) {
        return -1;
    }
    fp = fopen(m3u8_path, "rb");
    if (!fp) {
        return -1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    p = strstr(buf, "ZCS-HLS-GEN:");
    if (!p) {
        return -1; /* 外部静态包 */
    }
    return atoi(p + strlen("ZCS-HLS-GEN:"));
}

int zcs_vod_hls_playlist_stale(const char *m3u8_path)
{
    int gen = zcs_vod_hls_playlist_gen(m3u8_path);

    return gen >= 0 && gen < ZCS_VOD_HLS_GEN;
}

/*
 * 把请求里的分片相对路径解析到磁盘上的实际 .ts。
 * 依次尝试三种形态：
 *   1. 相对路径原样命中（清单分片与源在同目录）；
 *   2. 取文件名里的序号，在“媒体目录”下拼 {序号}.ts（清单分片以裸序号命名）；
 *   3. 保留原目录结构、仅把文件名换成 {序号}.ts（子目录播放列表）。
 * 之所以要回退而非单一规则：不同打包器写清单的 URI 形态不同，只有逐个试
 * 才能都兼容。任一命中且文件合法即返回。
 */
int zcs_vod_hls_resolve_segment_file(const char *app, const char *stream, const char *rel,
                                     char *out, size_t out_cap)
{
    char dir[512];
    char alt[128];
    unsigned long long seg_no;
    const char *base;
    const char *slash;
    size_t n;

    (void)alt;
    if (!app || !rel || !out || out_cap == 0 || !rel_path_is_ts(rel)) {
        return 0;
    }
    if (zcs_vod_resolve_rel_path(app, rel, out, out_cap) && segment_file_valid(out)) {
        return 1;
    }

    slash = strrchr(rel, '/');
    base = slash ? slash + 1 : rel;
    seg_no = strtoull(base, NULL, 10);
    if (seg_no == 0 && base[0] != '0') {
        return 0;
    }
    if (!stream || !stream[0] || !vod_hls_media_dir(app, stream, dir, sizeof(dir))) {
        return 0;
    }
    n = (size_t)snprintf(out, out_cap, "%s/%llu.ts", dir, seg_no);
    if (n > 0 && n < out_cap && segment_file_valid(out)) {
        return 1;
    }
    snprintf(alt, sizeof(alt), "%llu.ts", seg_no);
    if (slash) {
        char rel_alt[ZCS_STREAM_MAX];
        size_t plen = (size_t)(slash - rel);
        if (plen + strlen(alt) + 2 >= sizeof(rel_alt)) {
            return 0;
        }
        memcpy(rel_alt, rel, plen);
        rel_alt[plen] = '/';
        strcpy(rel_alt + plen + 1, alt);
        if (zcs_vod_resolve_rel_path(app, rel_alt, out, out_cap) && segment_file_valid(out)) {
            return 1;
        }
    }
    return 0;
}

/* 是否需要在磁盘上创建目录（含缺失的上级）。逐级向上补建。
 *
 * skip 的两条判定：
 *   - tmp 为空串：路径以分隔符开头（如 "/data/..."）时第一段为空，不能 mkdir("");
 *   - 长度为 2 且形如 "C:"：Windows 盘符根，既已存在又不可 mkdir，跳过。 */
static int vod_hls_ensure_dir(const char *dir)
{
    struct stat st;
    char tmp[512];
    char *p;

    if (!dir || !dir[0]) {
        return 0;
    }
    if (stat(dir, &st) == 0) {
        return zcs_vod_is_dir(st) ? 1 : 0;
    }
    /* create missing ancestors first (e.g. ./www/vod before ./www/vod/sub) */
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 1; *p; ++p) {
        if (*p != '/' && *p != '\\') {
            continue;
        }
        char sep = *p;
        *p = '\0';
        int skip = tmp[0] == '\0' || (strlen(tmp) == 2 && tmp[1] == ':');
        if (!skip && stat(tmp, &st) != 0) {
            (void)zcs_vod_mkdir(tmp);
        }
        *p = sep;
    }
    if (stat(dir, &st) == 0) {
        return zcs_vod_is_dir(st) ? 1 : 0;
    }
    return zcs_vod_mkdir(dir) == 0;
}

int zcs_vod_hls_playlist_path(const char *app, const char *stream, char *out, size_t out_cap)
{
    char m3u8_rel[ZCS_STREAM_MAX];

    if (!zcs_vod_mp4_stream_to_m3u8_rel(stream, m3u8_rel, sizeof(m3u8_rel))) {
        return 0;
    }
    return zcs_vod_resolve_rel_path(app, m3u8_rel, out, out_cap);
}

int zcs_vod_hls_segment_path(const char *app, const char *stream, uint64_t seg_no, char *out,
                             size_t out_cap)
{
    char dir[512];
    size_t n;

    if (!vod_hls_media_dir(app, stream, dir, sizeof(dir))) {
        return 0;
    }
    n = (size_t)snprintf(out, out_cap, "%s/%llu.ts", dir, (unsigned long long)seg_no);
    return n > 0 && n < out_cap;
}

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    int done;
} vod_hls_seg_capture;

typedef struct {
    const zcs_container_muxer_ops *ts_mux_ops;
    void *ts_mux;
    uint8_t *annexb;
    size_t annexb_cap;
    uint8_t *mux_buf;
    size_t mux_buf_cap;
    uint8_t *adts;
    size_t adts_cap;
    zcs_codec_param_sets params;
    uint8_t hevc_esinfo[32];
    size_t hevc_esinfo_len;
    zcs_aac_config aac;
    zcs_codec_id video_codec;
    int have_aac;
    int sent_video_cfg;
    int sent_audio_cfg;
    int video_armed;
    int enable_audio;
    int logged_sync;
    uint32_t last_mux_dts_ms;
    vod_hls_seg_capture *cap;
} vod_hls_mux;

static int vod_hls_segment_cb(void *param, const void *data, size_t bytes, int64_t pts, int64_t dts,
                              int64_t duration)
{
    vod_hls_seg_capture *cap = (vod_hls_seg_capture *)param;
    (void)pts;
    (void)dts;
    (void)duration;
    if (!cap || cap->done || !data || bytes == 0) {
        return 0;
    }
    if (bytes > cap->cap) {
        uint8_t *p = (uint8_t *)realloc(cap->data, bytes);
        if (!p) {
            return -1;
        }
        cap->data = p;
        cap->cap = bytes;
    }
    memcpy(cap->data, data, bytes);
    cap->len = bytes;
    cap->done = 1;
    return 0;
}

static void vod_ts_mux_destroy(vod_hls_mux *m)
{
    if (!m) {
        return;
    }
    if (m->ts_mux_ops && m->ts_mux) {
        m->ts_mux_ops->destroy(m->ts_mux);
    }
    m->ts_mux = NULL;
    m->ts_mux_ops = NULL;
}

static void vod_ts_set_extradata(vod_hls_mux *m, int stream_type, const void *extra, size_t len)
{
    if (m && m->ts_mux_ops && m->ts_mux && m->ts_mux_ops->set_extradata) {
        m->ts_mux_ops->set_extradata(m->ts_mux, stream_type, extra, len);
    }
}

static void vod_ts_write_frame(vod_hls_mux *m, int stream_type, const void *data, size_t len,
                               int64_t pts_ms, int64_t dts_ms, int flags)
{
    if (m && m->ts_mux_ops && m->ts_mux && m->ts_mux_ops->write_frame) {
        (void)m->ts_mux_ops->write_frame(m->ts_mux, stream_type, data, len, pts_ms, dts_ms, flags);
    }
}

static void vod_ts_flush(vod_hls_mux *m, int stream_type, int64_t pts_ms)
{
    if (m && m->ts_mux_ops && m->ts_mux && m->ts_mux_ops->flush) {
        m->ts_mux_ops->flush(m->ts_mux, stream_type, pts_ms);
    }
}

/** PMT ES_info：Registration 'HEVC' + HEVC video descriptor */
static int vod_hls_hevc_ts_esinfo(const uint8_t *hvcc, size_t hvcc_len, uint8_t *out, size_t cap)
{
    struct mpeg4_hevc_t hevc;
    uint64_t v;
    uint64_t copied44;
    uint8_t *p;

    if (!hvcc || hvcc_len < 7 || !out || cap < 6 + 2 + 13) {
        return -1;
    }
    memset(&hevc, 0, sizeof(hevc));
    if (mpeg4_hevc_decoder_configuration_record_load(hvcc, hvcc_len, &hevc) <= 0) {
        return -1;
    }

    p = out;
    *p++ = 0x05;
    *p++ = 4;
    memcpy(p, "HEVC", 4);
    p += 4;

    *p++ = 0x38;
    *p++ = 13;
    *p++ = (uint8_t)((hevc.general_profile_space << 6) | (hevc.general_tier_flag << 5) |
                     (hevc.general_profile_idc & 0x1f));
    p[0] = (uint8_t)((hevc.general_profile_compatibility_flags >> 24) & 0xff);
    p[1] = (uint8_t)((hevc.general_profile_compatibility_flags >> 16) & 0xff);
    p[2] = (uint8_t)((hevc.general_profile_compatibility_flags >> 8) & 0xff);
    p[3] = (uint8_t)(hevc.general_profile_compatibility_flags & 0xff);
    p += 4;

    copied44 = (hevc.general_constraint_indicator_flags >> 4) & 0xFFFFFFFFFFFULL;
    v = (1ULL << 63) | (1ULL << 60) | (copied44 << 16) | ((uint64_t)hevc.general_level_idc << 8);
    p[0] = (uint8_t)((v >> 56) & 0xff);
    p[1] = (uint8_t)((v >> 48) & 0xff);
    p[2] = (uint8_t)((v >> 40) & 0xff);
    p[3] = (uint8_t)((v >> 32) & 0xff);
    p[4] = (uint8_t)((v >> 24) & 0xff);
    p[5] = (uint8_t)((v >> 16) & 0xff);
    p[6] = (uint8_t)((v >> 8) & 0xff);
    p[7] = (uint8_t)(v & 0xff);
    return (int)(p + 8 - out);
}

static void feed_video_cfg(vod_hls_mux *m, const uint8_t *cfg, size_t clen)
{
    zcs_codec_id vc;

    if (!m || !cfg || clen < 2) {
        return;
    }
    vc = zcs_flv_tag_video_codec(cfg, clen);
    m->video_codec = vc;
    if (vc == ZCS_CODEC_H264) {
        const uint8_t *avcc = NULL;
        size_t avcc_len = 0;

        (void)zcs_codec_param_sets_cache_flv_cfg(&m->params, cfg, clen);
        if (m->ts_mux && zcs_rtmp_avc_extradata(cfg, clen, &avcc, &avcc_len)) {
            vod_ts_set_extradata(m, zcs_codec_mpeg_psi(ZCS_CODEC_H264), avcc, avcc_len);
        }
    } else if (vc == ZCS_CODEC_H265) {
        const uint8_t *hvcc = NULL;
        size_t hvcc_len = 0;

        (void)zcs_codec_param_sets_cache_flv_cfg(&m->params, cfg, clen);
        if (m->ts_mux && zcs_h265_video_config_hvcc(cfg, clen, &hvcc, &hvcc_len)) {
            m->hevc_esinfo_len = 0;
            if (hvcc && hvcc_len > 0) {
                int es_len =
                    vod_hls_hevc_ts_esinfo(hvcc, hvcc_len, m->hevc_esinfo, sizeof(m->hevc_esinfo));
                if (es_len > 0) {
                    m->hevc_esinfo_len = (size_t)es_len;
                }
            }
            if (m->hevc_esinfo_len > 0) {
                vod_ts_set_extradata(m, zcs_codec_mpeg_psi(ZCS_CODEC_H265), m->hevc_esinfo,
                                     m->hevc_esinfo_len);
            } else {
                vod_ts_set_extradata(m, zcs_codec_mpeg_psi(ZCS_CODEC_H265), hvcc, hvcc_len);
            }
        }
    }
}

static void feed_audio_cfg(vod_hls_mux *m, const uint8_t *cfg, size_t clen)
{
    const uint8_t *asc = NULL;
    size_t asc_len = 0;

    if (!m || !cfg || clen < 4) {
        return;
    }
    if (zcs_flv_tag_audio_codec(cfg, clen) != ZCS_CODEC_AAC) {
        return;
    }
    if (cfg[1] == 0) {
        int sr = 0;
        int ch = 0;
        (void)zcs_aac_parse_asc(cfg + 2, clen - 2, &sr, &ch);
        if (!zcs_rtmp_aac_extradata(cfg, clen, &asc, &asc_len)) {
            return;
        }
        zcs_aac_config_set_defaults(&m->aac, 44100, 2);
        if (zcs_aac_config_load_asc(&m->aac, asc, asc_len)) {
            m->have_aac = 1;
        }
        if (m->ts_mux) {
            vod_ts_set_extradata(m, zcs_codec_mpeg_psi(ZCS_CODEC_AAC), asc, asc_len);
        }
    }
}

static int aac_es_is_adts(const uint8_t *es, size_t es_len)
{
    return es && es_len >= 7 && es[0] == 0xff && (es[1] & 0xf0) == 0xf0;
}

/*
 * 送入一帧 H264 ES 到 TS muxer。
 *
 * 首帧必须是关键帧（IDR）才 armed：HLS 播放端从任意位置进入分片，非关键帧
 * 起头无法解码。armed 前一律丢弃，同理 feed_audio_es 也要求 video_armed。
 * armed 后的关键帧要补回 SPS/PPS 前缀：分片/seek 后的 IDR 可能不自带参数集，
 * 播放端拿不到 SPS/PPS 就解不了，故从缓存拼接。
 */
static void feed_h264_video_es(vod_hls_mux *m, const uint8_t *annexb, size_t len, uint32_t ts,
                               int keyframe)
{
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;

    if (!m || !m->ts_mux || !annexb || len < 4) {
        return;
    }
    if (!m->video_armed) {
        if (!keyframe) {
            return;
        }
        m->video_armed = 1;
        if (!m->logged_sync) {
            m->logged_sync = 1;
            zn_info("vod hls: first H264 IDR at ts=%u", (unsigned)ts);
        }
    }
    if (keyframe && m->params.sps && m->params.pps) {
        size_t full_len = 0;
        if (zcs_h264_annexb_prepend_sps_pps(m->params.sps, m->params.sps_len, m->params.pps,
                                            m->params.pps_len, annexb, len, m->mux_buf,
                                            m->mux_buf_cap, &full_len) == ZN_OK &&
            full_len) {
            mux_ptr = m->mux_buf;
            mux_len = full_len;
        }
    }
    m->last_mux_dts_ms = ts;
    vod_ts_write_frame(m, zcs_codec_mpeg_psi(ZCS_CODEC_H264), mux_ptr, mux_len, (int64_t)ts,
                       (int64_t)ts, keyframe ? ZCS_CONTAINER_MUX_FLAG_KEYFRAME : 0);
}

static void feed_h265_video_es(vod_hls_mux *m, const uint8_t *annexb, size_t len, uint32_t ts,
                               int keyframe)
{
    const uint8_t *mux_ptr = annexb;
    size_t mux_len = len;
    int sync;

    if (!m || !m->ts_mux || !annexb || len < 4) {
        return;
    }

    sync = keyframe || zcs_h265_annexb_is_sync_key(annexb, len);
    if (!m->video_armed) {
        if (!sync) {
            return;
        }
        m->video_armed = 1;
        if (!m->logged_sync) {
            m->logged_sync = 1;
            zn_info("vod hls: first H265 sync at ts=%u", (unsigned)ts);
        }
    }

    if (sync && m->params.sps_len > 0 && m->params.pps_len > 0) {
        size_t full_len = 0;
        if (zcs_h265_annexb_build_rtp_au(m->params.vps_len ? m->params.vps : NULL,
                                         m->params.vps_len, m->params.sps, m->params.sps_len,
                                         m->params.pps, m->params.pps_len, annexb, len, 1,
                                         m->mux_buf, m->mux_buf_cap, &full_len) == ZN_OK &&
            full_len > 0) {
            mux_ptr = m->mux_buf;
            mux_len = full_len;
        }
    } else {
        size_t vcl_len = 0;
        if (zcs_h265_annexb_copy_vcl(annexb, len, m->mux_buf, m->mux_buf_cap, &vcl_len) == ZN_OK &&
            vcl_len > 0) {
            mux_ptr = m->mux_buf;
            mux_len = vcl_len;
        }
    }

    m->last_mux_dts_ms = ts;
    vod_ts_write_frame(m, zcs_codec_mpeg_psi(ZCS_CODEC_H265), mux_ptr, mux_len, (int64_t)ts,
                       (int64_t)ts, sync ? ZCS_CONTAINER_MUX_FLAG_KEYFRAME : 0);
}

/*
 * 送入一帧音频到 TS muxer。MPEG-TS 里的 AAC 必须是 ADTS 帧格式，而 VOD 源
 * 里常是裸 AAC（无 ADTS 头）。故非 ADTS 的裸帧需现场加 ADTS 头（7 字节，
 * 从 ASC 推出 profile/采样率/声道）。es_len > 0x1fff 时放弃：ADTS 的帧长字段
 * 只有 13 位，超长无法表达，强塞会产出非法帧。
 */
static void feed_audio_es(vod_hls_mux *m, const uint8_t *es, size_t es_len, uint32_t ts)
{
    const uint8_t *mux_ptr = es;
    size_t mux_len = es_len;

    if (!m || !m->ts_mux || !m->enable_audio || !es || es_len == 0 || !m->video_armed) {
        return;
    }
    if (!aac_es_is_adts(es, es_len)) {
        int hdr;
        if (!m->have_aac || es_len > 0x1fff) {
            return;
        }
        hdr = zcs_aac_config_adts_header(&m->aac, (uint16_t)es_len, m->adts, m->adts_cap);
        if (hdr < 7 || (size_t)hdr + es_len > m->adts_cap) {
            return;
        }
        memcpy(m->adts + hdr, es, es_len);
        mux_ptr = m->adts;
        mux_len = (size_t)hdr + es_len;
    }
    m->last_mux_dts_ms = ts;
    vod_ts_write_frame(m, zcs_codec_mpeg_psi(ZCS_CODEC_AAC), mux_ptr, mux_len, (int64_t)ts,
                       (int64_t)ts, 0);
}

static void vod_hls_mux_free(vod_hls_mux *m)
{
    int psi_video;

    if (!m) {
        return;
    }
    psi_video = zcs_codec_mpeg_psi(m->video_codec);
    if (!psi_video) {
        psi_video = zcs_codec_mpeg_psi(ZCS_CODEC_H264);
    }
    if (m->ts_mux && m->video_armed) {
        vod_ts_flush(m, psi_video, (int64_t)m->last_mux_dts_ms);
    }
    vod_ts_mux_destroy(m);
    free(m->annexb);
    free(m->mux_buf);
    free(m->adts);
    zcs_codec_param_sets_clear(&m->params);
    memset(m, 0, sizeof(*m));
}

/*
 * 原子写入分片：先写 path.tmp，成功后 remove(path) 再 rename。
 *
 * 为什么不能直接 fopen(path, "wb")：HTTP 客户端可能正在并发下载这个分片，
 * 直接覆盖会读到半截文件（长度未知、内容错位）。rename 在同一文件系统内是
 * 原子替换，读者要么看到旧的完整文件、要么看到新的完整文件，绝不会看到中间态。
 * 先 remove 是 Windows 兼容：目标存在时 rename 会失败，需先删。
 */
static zn_err_t write_segment_file(const char *path, const void *data, size_t len)
{
    char tmp[560];
    FILE *fp;
    size_t n;

    n = (size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n == 0 || n >= sizeof(tmp)) {
        return ZN_ERR_INVALID;
    }
    fp = fopen(tmp, "wb");
    if (!fp) {
        return ZN_ERR_IO;
    }
    if (fwrite(data, 1, len, fp) != len) {
        fclose(fp);
        remove(tmp);
        return ZN_ERR_IO;
    }
    fclose(fp);
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return ZN_ERR_IO;
    }
    return ZN_OK;
}

/*
 * 生成第 seg_no 个分片。
 *
 * 起点对齐 FLV 索引里的关键帧时间（idx->times[seg_no]），终点取下一个索引点
 * （末片取总时长），保证分片边界与清单 EXTINF 严格一致——这是 A/V 同步与
 * “按序号取片”能自洽的前提。
 *
 * 循环的双重退出条件（loops 上限 + idle 上限）是防御性的：正常靠 muxer 的
 * cap.done（本片封好）退出；但若源数据异常（读不到、无关键帧），不能让生成
 * 过程无限转，空闲到 128 轮就放弃，交由调用方报错。
 */
static zn_err_t generate_segment(zcs_media_source *src, uint64_t seg_no, zn_poller *poller,
                                  const char *out_path)
{
    const zcs_vod_flv_index *idx;
    zcs_vod_play_lane *lane = NULL;
    zcs_vod_buffer_reader *rd;
    zcs_vod_reader *reader;
    vod_hls_mux mux;
    vod_hls_seg_capture cap = {0};
    uint64_t start_ms;
    uint64_t dur_ms;
    size_t seg_count;
    int is_last;
    int idle;
    int loops;

    idx = zcs_vod_source_flv_index(src);
    if (!idx || idx->count == 0) {
        return ZN_ERR_INVALID;
    }
    seg_count = idx->count;
    if (seg_no >= seg_count) {
        return ZN_ERR_INVALID;
    }

    dur_ms = zcs_vod_source_duration_ms(src);
    start_ms = (uint64_t)(idx->times[seg_no] * 1000.0);
    is_last = (seg_no + 1 >= seg_count);

    lane = zcs_vod_play_lane_open(src, poller);
    if (!lane) {
        return ZN_ERR_NOMEM;
    }
    (void)zcs_vod_play_lane_prepare(lane, start_ms);
    zcs_vod_play_lane_seek_ms(lane, start_ms);
    zcs_vod_play_lane_prefill(lane);
    zcs_vod_play_lane_align_reader(lane);
    rd = zcs_vod_play_lane_buffer_reader(lane);
    reader = zcs_vod_play_lane_reader(lane);
    if (!rd || !reader) {
        zcs_vod_play_lane_close(lane);
        return ZN_ERR_INVALID;
    }

    memset(&mux, 0, sizeof(mux));
    mux.enable_audio = src->has_audio;
    mux.cap = &cap;
    mux.annexb_cap = 256 * 1024;
    mux.mux_buf_cap = mux.annexb_cap;
    mux.adts_cap = 8 * 1024;
    mux.annexb = (uint8_t *)malloc(mux.annexb_cap);
    mux.mux_buf = (uint8_t *)malloc(mux.mux_buf_cap);
    mux.adts = (uint8_t *)malloc(mux.adts_cap);
    if (!mux.annexb || !mux.mux_buf || !mux.adts) {
        vod_hls_mux_free(&mux);
        free(cap.data);
        zcs_vod_play_lane_close(lane);
        return ZN_ERR_NOMEM;
    }
    /* duration=0：每个关键帧切一片；seek 到第 N 个关键帧后只保留第一个 */
    {
        zcs_container_mux_opts mcfg;
        /* 切片时长必须与清单 EXTINF 一致（索引相邻关键帧的时间网格），否则
         * libhls 会按源文件真实关键帧间隔（可能远小于网格，如 0.5s）切段：
         * 清单声明 2s 而分段只有 0.5s，播放器每 2s 断粮一次 → 一卡一卡。 */
        uint64_t end_ms =
            is_last ? dur_ms : (uint64_t)(idx->times[seg_no + 1] * 1000.0 + 0.5);
        int64_t seg_ms = (int64_t)end_ms - (int64_t)start_ms;

        if (seg_ms < 200) {
            seg_ms = 200;
        }
        if (seg_ms > 30000) {
            seg_ms = 30000;
        }

        memset(&mcfg, 0, sizeof(mcfg));
        mcfg.id = ZCS_CONTAINER_MPEGTS;
        mcfg.segment_duration_ms = seg_ms;
        mcfg.on_segment = vod_hls_segment_cb;
        mcfg.user = &cap;
        mux.ts_mux_ops = zcs_container_muxer_find(ZCS_CONTAINER_MPEGTS);
        mux.ts_mux =
            mux.ts_mux_ops && mux.ts_mux_ops->create ? mux.ts_mux_ops->create(&mcfg) : NULL;
    }
    if (!mux.ts_mux) {
        vod_hls_mux_free(&mux);
        free(cap.data);
        zcs_vod_play_lane_close(lane);
        return ZN_ERR_NOMEM;
    }

    idle = 0;
    loops = 0;
    while (loops < 500000 && idle < 128) {
        zcs_gop_slot slot;
        int n;
        int got_slot = 0;

        if (!is_last && cap.done) {
            break;
        }

        for (n = 0; n < 32; ++n) {
            if (zcs_vod_reader_pump(reader) <= 0) {
                break;
            }
        }
        if (!mux.sent_video_cfg) {
            size_t clen = 0;
            const uint8_t *cfg = zcs_vod_play_lane_video_config(lane, &clen);
            if (cfg && clen) {
                feed_video_cfg(&mux, cfg, clen);
                mux.sent_video_cfg = 1;
            }
        }
        if (!mux.sent_audio_cfg && mux.enable_audio) {
            size_t clen = 0;
            const uint8_t *cfg = zcs_vod_play_lane_audio_config(lane, &clen);
            if (cfg && clen) {
                feed_audio_cfg(&mux, cfg, clen);
                mux.sent_audio_cfg = 1;
            } else if (clen == 0) {
                mux.sent_audio_cfg = 1;
            }
        }

        n = 0;
        while (n < 64 && zcs_vod_buffer_reader_peek_muxed(rd, &slot) > 0) {
            if (!is_last && cap.done) {
                break;
            }
            if (!slot.data || slot.len == 0) {
                zcs_vod_buffer_reader_advance(rd);
                continue;
            }
            if (slot.track == ZCS_TRACK_VIDEO) {
                int sync = slot.keyframe;

                if (slot.codec == ZCS_CODEC_H265) {
                    sync = sync || zcs_h265_annexb_is_sync_key(slot.data, slot.len);
                }
                if (!mux.video_armed && !sync) {
                    zcs_vod_buffer_reader_advance(rd);
                    continue;
                }
                if (slot.codec == ZCS_CODEC_H265) {
                    feed_h265_video_es(&mux, slot.data, slot.len, slot.dts_ms, slot.keyframe);
                } else {
                    feed_h264_video_es(&mux, slot.data, slot.len, slot.dts_ms, slot.keyframe);
                }
            } else if (slot.track == ZCS_TRACK_AUDIO) {
                if (!mux.video_armed) {
                    zcs_vod_buffer_reader_advance(rd);
                    continue;
                }
                feed_audio_es(&mux, slot.data, slot.len, slot.dts_ms);
            }
            zcs_vod_buffer_reader_advance(rd);
            idle = 0;
            got_slot = 1;
            n++;
            if (!is_last && cap.done) {
                break;
            }
        }
        if (!got_slot) {
            idle++;
        }
        loops++;
    }

    if (is_last && !cap.done && mux.video_armed) {
        int psi_video = zcs_codec_mpeg_psi(mux.video_codec);
        if (!psi_video) {
            psi_video = zcs_codec_mpeg_psi(ZCS_CODEC_H264);
        }
        vod_ts_flush(&mux, psi_video, (int64_t)mux.last_mux_dts_ms);
    }

    vod_hls_mux_free(&mux);
    zcs_vod_play_lane_close(lane);

    if (!cap.done || cap.len < 188) {
        free(cap.data);
        zn_warn("vod hls: segment generate failed %s/%s seg=%llu start=%llu ms", src->app,
                 src->stream, (unsigned long long)seg_no, (unsigned long long)start_ms);
        return ZN_ERR_INVALID;
    }
    if (write_segment_file(out_path, cap.data, cap.len) != ZN_OK) {
        free(cap.data);
        return ZN_ERR_IO;
    }
    free(cap.data);
    zn_info("vod hls: generated %s (%u bytes, start=%llu ms, last=%d)", out_path,
             (unsigned)cap.len, (unsigned long long)start_ms, is_last);
    (void)dur_ms;
    return ZN_OK;
}

static void vod_hls_purge_segments(const char *dir)
{
#if defined(_WIN32)
    char pattern[560];
    struct _finddata_t fd;
    intptr_t h;

    if (!dir || !dir[0]) {
        return;
    }
    snprintf(pattern, sizeof(pattern), "%s/*.ts", dir);
    h = _findfirst(pattern, &fd);
    if (h == -1) {
        return;
    }
    do {
        char path[560];
        if (fd.name[0] == '.') {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
        remove(path);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d;
    struct dirent *ent;

    if (!dir || !dir[0]) {
        return;
    }
    d = opendir(dir);
    if (!d) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        char path[560];
        size_t nlen;
        if (ent->d_name[0] == '.') {
            continue;
        }
        nlen = strlen(ent->d_name);
        if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".ts") != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
        remove(path);
    }
    closedir(d);
#endif
}

/*
 * 判断现有清单能否复用：mtime 不早于源 mp4，且正文含当前代际标记。
 * 代际标记缺失说明是外部静态包或旧版本产物；这里只看“是否包含当前 gen 行”，
 * 旧 gen 的行自然不匹配 → 触发重建（stale 判定另有函数）。
 */
static int playlist_cache_valid(const char *m3u8, time_t mp4_t)
{
    char buf[4096];
    FILE *fp;
    time_t m3u8_t;
    size_t n;

    if (!segment_file_valid(m3u8) || !file_mtime(m3u8, &m3u8_t) || m3u8_t < mp4_t) {
        return 0;
    }
    fp = fopen(m3u8, "rb");
    if (!fp) {
        return 0;
    }
    n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) {
        return 0;
    }
    buf[n] = '\0';
    return strstr(buf, ZCS_VOD_HLS_GEN_LINE) != NULL;
}

static unsigned zcs_hls_target_duration_sec(double sec)
{
    unsigned u;

    if (sec < 1.0) {
        return 1u;
    }
    u = (unsigned)sec;
    return (sec > (double)u) ? u + 1u : u;
}

/* 清单里每片的 EXTINF 必须用“索引相邻关键帧时间差”，与 generate_segment 里
 * 切片用的时长同源（都取自 idx->times）。二者若不一致，播放器会按 EXTINF 期待
 * 时长去取到长短不符的分片，导致卡顿或过早/过晚切片。末片的 t1 取总时长。 */
static zn_err_t build_playlist_file(zcs_media_source *src, const char *path)
{
    const zcs_vod_flv_index *idx;
    char tmp[560];
    FILE *fp;
    double dur_sec;
    double max_inf = 0.0;
    size_t i;

    idx = zcs_vod_source_flv_index(src);
    if (!idx || idx->count == 0) {
        return ZN_ERR_INVALID;
    }
    dur_sec = zcs_vod_source_duration_ms(src) / 1000.0;
    if (dur_sec <= 0.0) {
        return ZN_ERR_INVALID;
    }

    for (i = 0; i < idx->count; ++i) {
        double t0 = idx->times[i];
        double t1 = (i + 1 < idx->count) ? idx->times[i + 1] : dur_sec;
        double inf = t1 - t0;
        if (inf > max_inf) {
            max_inf = inf;
        }
    }
    if (max_inf < 1.0) {
        max_inf = 1.0;
    }

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) {
        return ZN_ERR_IO;
    }
    fprintf(fp,
            "#EXTM3U\r\n" ZCS_VOD_HLS_GEN_LINE "#EXT-X-VERSION:3\r\n#EXT-X-ALLOW-CACHE:YES\r\n");
    fprintf(fp, "#EXT-X-TARGETDURATION:%u\r\n", zcs_hls_target_duration_sec(max_inf));
    fprintf(fp, "#EXT-X-PLAYLIST-TYPE:VOD\r\n#EXT-X-MEDIA-SEQUENCE:0\r\n");
    for (i = 0; i < idx->count; ++i) {
        double t0 = idx->times[i];
        double t1 = (i + 1 < idx->count) ? idx->times[i + 1] : dur_sec;
        double inf = t1 - t0;
        if (inf < 0.001) {
            inf = 0.001;
        }
        fprintf(fp, "#EXTINF:%.3f,\r\n%zu.ts\r\n", inf, i);
    }
    fprintf(fp, "#EXT-X-ENDLIST\r\n");
    fclose(fp);
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return ZN_ERR_IO;
    }
    zn_info("vod hls: playlist %s segs=%zu target=%.1fs dur=%.1fs", path, idx->count, max_inf,
             dur_sec);
    return ZN_OK;
}

/*
 * 确保清单存在且与源文件一致。
 *
 * 两级缓存校验（无锁一次 + 持锁后一次）是典型的双检锁：多个请求可能同时
 * 发现清单过期而都想重建，无锁检查先挡掉大部分；真正重建放在 g_vod_hls_mu
 * 下，并在锁内再查一遍，避免第二个线程重复做昂贵的扫盘/切片。
 *
 * 重建前先 purge 旧 .ts：代际（ZCS_VOD_HLS_GEN）升级意味着时间戳策略变了，
 * 旧分片与新清单的时间基准不再兼容，不清会播出花屏/音画错位。
 */
zn_err_t zcs_vod_hls_ensure_playlist(zcs_media_source *src)
{
    char mp4[512];
    char m3u8[512];
    char dir[512];
    time_t mp4_t = 0;
    zn_err_t err;

    if (!src) {
        return ZN_ERR_INVALID;
    }

    {
        char static_m3u8[512];
        int gen;

        if (zcs_vod_hls_resolve_playlist(src->app, src->stream, static_m3u8,
                                         sizeof(static_m3u8))) {
            gen = zcs_vod_hls_playlist_gen(static_m3u8);
            /* 外部静态包（无代际标记）直接使用；本模块生成的旧代缓存继续走重建。 */
            if (!zcs_vod_hls_playlist_stale(static_m3u8)) {
                zn_info("vod hls: static playlist app=%s stream=%s path=%s gen=%d", src->app,
                         src->stream, static_m3u8, gen);
                return ZN_OK;
            }
            zn_info("vod hls: stale playlist gen=%d < %d, will rebuild: %s", gen, ZCS_VOD_HLS_GEN,
                     static_m3u8);
        }
    }

    if (!zcs_vod_source_file_path(src, mp4, sizeof(mp4)) &&
        !zcs_vod_resolve_file_path(src->app, src->stream, mp4, sizeof(mp4))) {
        return ZN_ERR_INVALID;
    }
    if (!zcs_vod_hls_playlist_path(src->app, src->stream, m3u8, sizeof(m3u8))) {
        return ZN_ERR_INVALID;
    }
    if (!vod_hls_media_dir(src->app, src->stream, dir, sizeof(dir))) {
        return ZN_ERR_INVALID;
    }
    if (!vod_hls_ensure_dir(dir)) {
        return ZN_ERR_IO;
    }

    if (file_mtime(mp4, &mp4_t) && playlist_cache_valid(m3u8, mp4_t)) {
        return ZN_OK;
    }

    if (!g_vod_hls_mu) {
        g_vod_hls_mu = zb_mutex_create(0);
    }
    if (!g_vod_hls_mu) {
        return ZN_ERR_NOMEM;
    }
    zb_mutex_lock(g_vod_hls_mu);
    if (file_mtime(mp4, &mp4_t) && playlist_cache_valid(m3u8, mp4_t)) {
        zb_mutex_unlock(g_vod_hls_mu);
        return ZN_OK;
    }
    vod_hls_purge_segments(dir);
    zn_info("vod hls: rebuild playlist (gen=%d), purged cached .ts under %s", ZCS_VOD_HLS_GEN,
             dir);
    err = build_playlist_file(src, m3u8);
    zb_mutex_unlock(g_vod_hls_mu);
    return err;
}

/*
 * 确保某个分片已生成到磁盘。同样是“快速返回 + 双检锁”结构：
 * 已存在且比源文件新（ts_t >= mp4_t）就直接用，否则在锁内重新生成。
 * 比较 mtime 而非内容：源 mp4 被替换后分片必然过期，mtime 是最廉价可靠
 * 的判据；分片比源新说明它就是从当前源生成的。
 */
zn_err_t zcs_vod_hls_ensure_segment(zcs_media_source *src, uint64_t seg_no, zn_poller *poller)
{
    char mp4[512];
    char ts_path[512];
    char dir[512];
    time_t mp4_t = 0;
    time_t ts_t = 0;
    zn_err_t err;

    if (!src) {
        return ZN_ERR_INVALID;
    }

    if (zcs_vod_hls_segment_path(src->app, src->stream, seg_no, ts_path, sizeof(ts_path)) &&
        segment_file_valid(ts_path)) {
        return ZN_OK; /* pre-packed or already generated segment */
    }

    if (!zcs_vod_source_file_path(src, mp4, sizeof(mp4)) &&
        !zcs_vod_resolve_file_path(src->app, src->stream, mp4, sizeof(mp4))) {
        return ZN_ERR_INVALID;
    }
    if (!zcs_vod_hls_segment_path(src->app, src->stream, seg_no, ts_path, sizeof(ts_path))) {
        return ZN_ERR_INVALID;
    }
    if (!vod_hls_media_dir(src->app, src->stream, dir, sizeof(dir))) {
        return ZN_ERR_INVALID;
    }
    if (!vod_hls_ensure_dir(dir)) {
        return ZN_ERR_IO;
    }
    if (zcs_vod_hls_ensure_playlist(src) != ZN_OK) {
        return ZN_ERR_INVALID;
    }

    if (segment_file_valid(ts_path) && file_mtime(mp4, &mp4_t) && file_mtime(ts_path, &ts_t) &&
        ts_t >= mp4_t) {
        return ZN_OK;
    }

    if (!g_vod_hls_mu) {
        g_vod_hls_mu = zb_mutex_create(0);
    }
    if (!g_vod_hls_mu) {
        return ZN_ERR_NOMEM;
    }
    zb_mutex_lock(g_vod_hls_mu);
    if (segment_file_valid(ts_path) && file_mtime(mp4, &mp4_t) && file_mtime(ts_path, &ts_t) &&
        ts_t >= mp4_t) {
        zb_mutex_unlock(g_vod_hls_mu);
        return ZN_OK;
    }
    err = generate_segment(src, seg_no, poller, ts_path);
    zb_mutex_unlock(g_vod_hls_mu);
    return err;
}
