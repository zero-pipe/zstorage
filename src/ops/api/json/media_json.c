/**
 * @file media_json.c
 * @brief webapi JSON 输出（媒体项/在线状态）。
 */
#include "zms/ops/api/json/media_json.h"
#include "zn/platform.h"
#include "zms/core/event.h"
#include "zms/egress/recorder/egress_segment_recorder.h"
#include "zms/media/codec/codec_id.h"
#include "zms/core/stream/url.h"
#include "zms/core/gop/queue.h"
#include "zms/core/stream/stats.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ZMS_DEFAULT_VHOST "__defaultVhost__"

/* zms_media_origin_str() 定义于 media_event.h */

static void fill_stream_stats(const zms_media_source *s, zms_media_stats_view *st)
{
    zms_media_stats_fill(s, s ? s->gop_queue : NULL, st);
}

static const zms_media_server_ports *resolve_ports(const zms_media_server_ports *ports)
{
    if (ports && (ports->rtmp || ports->rtsp || ports->http || ports->srt)) {
        return ports;
    }
    return zms_media_events_server_ports();
}

static int append_play_urls(zms_json_buf *jb, const zms_media_source *s,
                            const zms_media_server_ports *ports)
{
    const zms_media_server_ports *eff = resolve_ports(ports);
    if (!eff) {
        return zms_json_buf_append(jb, ",\"originUrl\":\"\"");
    }
    zms_media_urls u;
    zms_media_urls_build(&u, s, eff);
    return zms_json_buf_append(jb,
                               ",\"originUrl\":\"%s\""
                               ",\"srtUrl\":\"%s\""
                               ",\"rtmpUrl\":\"%s\""
                               ",\"rtspUrl\":\"%s\""
                               ",\"httpFlvUrl\":\"%s\""
                               ",\"hlsUrl\":\"%s\"",
                               u.origin, s->publish_origin == ZMS_ORIGIN_SRT_PUSH ? u.origin : "",
                               u.rtmp_play, u.rtsp_play, u.http_flv, u.hls);
}

int zms_json_buf_append(zms_json_buf *jb, const char *fmt, ...)
{
    if (!jb || !jb->buf || jb->len >= jb->cap) {
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= jb->cap - jb->len) {
        return -1;
    }
    jb->len += (size_t)n;
    return 0;
}

int zms_media_source_is_online(const zms_media_source *s)
{
    if (!s) {
        return 0;
    }
    if (s->vod_buffer) {
        return 1; /* VOD sources have no GOP ring; vod_buffer marks them active */
    }
    return s->gop_queue && (s->publishing || s->has_video || s->has_audio);
}

int zms_json_append_media_item(zms_json_buf *jb, zms_media_source *s, int *first,
                               const zms_media_server_ports *ports)
{
    if (!zms_media_source_is_online(s)) {
        return 0;
    }
    uint64_t now = zn_monotonic_ms();
    uint64_t alive = s->create_stamp_ms ? (now - s->create_stamp_ms) / 1000 : 0;
    zms_media_stats_view st;
    fill_stream_stats(s, &st);
    if (zms_json_buf_append(
            jb,
            "%s{"
            "\"schema\":\"%s\","
            "\"vhost\":\"%s\","
            "\"app\":\"%s\","
            "\"stream\":\"%s\","
            "\"streamRequested\":\"%s\","
            "\"createStamp\":%llu,"
            "\"currentStamp\":0,"
            "\"aliveSecond\":%llu,"
            "\"bytesSpeed\":%lld,"
            "\"totalBytes\":%llu,"
            "\"egressSpeed\":%lld,"
            "\"egressBytes\":%llu,"
            "\"frameRingPending\":%zu,"
            "\"frameRingMaxLag\":%zu,"
            "\"frameRingReaders\":%d,"
            "\"gopCount\":%zu,"
            "\"gopBytes\":%zu,"
            "\"readerCount\":%d,"
            "\"totalReaderCount\":%d,"
            "\"videoFps\":%u,"
            "\"audioFps\":%u,"
            "\"droppedFrames\":%llu,"
            "\"width\":%d,"
            "\"height\":%d,"
            "\"fps\":%.2f,"
            "\"sampleRate\":%d,"
            "\"channels\":%d,"
            "\"videoCodec\":\"%s\","
            "\"audioCodec\":\"%s\","
            "\"originType\":%d,"
            "\"originTypeStr\":\"%s\","
            "\"isRecordingMP4\":%s,"
            "\"isRecordingHLS\":%s,"
            "\"video\":%s,"
            "\"audio\":%s",
            *first ? "" : ",", s->schema, s->vhost, s->app, s->stream,
            s->stream_requested[0] ? s->stream_requested : s->stream,
            (unsigned long long)s->create_stamp_ms, (unsigned long long)alive,
            (long long)st.bytes_speed, (unsigned long long)st.ingress_bytes,
            (long long)st.egress_speed, (unsigned long long)st.egress_bytes, st.gop_queue_pending,
            st.gop_queue_max_lag, st.gop_queue_readers, st.gop_queue_gop_count,
            s->gop_queue ? zms_gop_queue_bytes(s->gop_queue) : (size_t)0, s->reader_count,
            s->reader_count, (unsigned)st.video_fps, (unsigned)st.audio_fps,
            (unsigned long long)st.dropped_frames, s->has_video ? s->video.width : 0,
            s->has_video ? s->video.height : 0, s->has_video ? (s->video.fps > 0.0 ? (double)s->video.fps : (double)st.video_fps) : 0.0,
            s->has_audio ? s->audio.sample_rate : 0, s->has_audio ? s->audio.channels : 0,
            s->has_video ? zms_codec_name(s->video.codec) : "",
            s->has_audio ? zms_codec_name(s->audio.codec) : "", s->publish_origin,
            zms_media_origin_str(s->publish_origin),
            s->mp4_recorder ? "true" : "false",
            zms_media_source_segment_rec_get(s, ZMS_SEGMENT_REC_HLS) ? "true" : "false",
            s->has_video ? "true" : "false", s->has_audio ? "true" : "false") != 0) {
        return -1;
    }
    if (append_play_urls(jb, s, ports) != 0) {
        return -1;
    }
    if (zms_json_buf_append(jb, "}") != 0) {
        return -1;
    }
    *first = 0;
    return 0;
}

int zms_json_write_media_info(zms_json_buf *jb, zms_media_source *s,
                              const zms_media_server_ports *ports)
{
    if (!jb || !s) {
        return -1;
    }
    if (zms_json_buf_append(jb, "{\"code\":0") != 0) {
        return -1;
    }
    if (!zms_media_source_is_online(s)) {
        return -1;
    }
    uint64_t now = zn_monotonic_ms();
    uint64_t alive = s->create_stamp_ms ? (now - s->create_stamp_ms) / 1000 : 0;
    zms_media_stats_view st;
    fill_stream_stats(s, &st);
    if (zms_json_buf_append(
            jb,
            ","
            "\"schema\":\"%s\","
            "\"vhost\":\"%s\","
            "\"app\":\"%s\","
            "\"stream\":\"%s\","
            "\"streamRequested\":\"%s\","
            "\"createStamp\":%llu,"
            "\"currentStamp\":0,"
            "\"aliveSecond\":%llu,"
            "\"bytesSpeed\":%lld,"
            "\"totalBytes\":%llu,"
            "\"egressSpeed\":%lld,"
            "\"egressBytes\":%llu,"
            "\"readerCount\":%d,"
            "\"totalReaderCount\":%d,"
            "\"frameRingPending\":%zu,"
            "\"frameRingMaxLag\":%zu,"
            "\"frameRingReaders\":%d,"
            "\"gopCount\":%zu,"
            "\"gopBytes\":%zu,"
            "\"videoFps\":%u,"
            "\"audioFps\":%u,"
            "\"droppedFrames\":%llu,"
            "\"width\":%d,"
            "\"height\":%d,"
            "\"fps\":%.2f,"
            "\"sampleRate\":%d,"
            "\"channels\":%d,"
            "\"videoCodec\":\"%s\","
            "\"audioCodec\":\"%s\","
            "\"originType\":%d,"
            "\"originTypeStr\":\"%s\","
            "\"isRecordingMP4\":%s,"
            "\"isRecordingHLS\":%s,"
            "\"video\":%s,"
            "\"audio\":%s",
            s->schema, s->vhost, s->app, s->stream,
            s->stream_requested[0] ? s->stream_requested : s->stream,
            (unsigned long long)s->create_stamp_ms, (unsigned long long)alive,
            (long long)st.bytes_speed, (unsigned long long)st.ingress_bytes,
            (long long)st.egress_speed, (unsigned long long)st.egress_bytes, s->reader_count,
            s->reader_count, st.gop_queue_pending, st.gop_queue_max_lag, st.gop_queue_readers,
            st.gop_queue_gop_count, s->gop_queue ? zms_gop_queue_bytes(s->gop_queue) : (size_t)0,
            (unsigned)st.video_fps, (unsigned)st.audio_fps,
            (unsigned long long)st.dropped_frames, s->has_video ? s->video.width : 0,
            s->has_video ? s->video.height : 0, s->has_video ? (s->video.fps > 0.0 ? (double)s->video.fps : (double)st.video_fps) : 0.0,
            s->has_audio ? s->audio.sample_rate : 0, s->has_audio ? s->audio.channels : 0,
            s->has_video ? zms_codec_name(s->video.codec) : "",
            s->has_audio ? zms_codec_name(s->audio.codec) : "", s->publish_origin,
            zms_media_origin_str(s->publish_origin),
            s->mp4_recorder ? "true" : "false",
            zms_media_source_segment_rec_get(s, ZMS_SEGMENT_REC_HLS) ? "true" : "false",
            s->has_video ? "true" : "false", s->has_audio ? "true" : "false") != 0) {
        return -1;
    }
    if (append_play_urls(jb, s, ports) != 0) {
        return -1;
    }
    if (zms_json_buf_append(jb, "}") != 0) {
        return -1;
    }
    return 0;
}
