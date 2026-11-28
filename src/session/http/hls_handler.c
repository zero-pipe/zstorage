/**
 * @file hls_handler.c
 * @brief 直播 HLS 清单/分片服务。
 */
#include "zcs/play/http_port.h"
#include "session/http/session_internal.h"
#include "zcs/core/codec_filter.h"
#include "zcs/egress/recorder/egress_segment_recorder.h"
#include "zcs/core/event.h"
#include "zn/util/buf.h"
#include "zn/util/log.h"
#include <string.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

static void hls_http_done(zcs_http_hls_segmenter *rec)
{
    zcs_http_hls_segmenter_http_leave(rec);
}

static void hls_http_done_request(zcs_http_session *hs, zcs_http_hls_segmenter *rec)
{
    (void)hs;
    if (rec) {
        hls_http_done(rec);
    }
}

void zcs_http_live_hls_handle(zcs_http_session *hs, zcs_media_source *src, const char *app,
                             const char *stream, const char *file)
{
    zcs_http_hls_segmenter *rec;
    size_t m3u8_len = 0;
    size_t ts_len = 0;
    int seg_count = 0;

    if (!hs || !src || !src->gop_queue) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    if (zcs_session_capability_check_source(ZCS_PROTO_CAP_HLS_PLAY, src) != ZN_OK) {
        zcs_session_capability_log_reject("hls", src, ZCS_PROTO_CAP_HLS_PLAY);
        zcs_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }

    zcs_http_hls_ensure_recorder(src, zcs_http_hls_main_poller());
    rec = (zcs_http_hls_segmenter *)zcs_media_source_segment_rec_get(src, ZCS_SEGMENT_REC_HLS);
    if (!rec) {
        zn_warn("HLS live 404: recorder unavailable app=%s stream=%s (see HLS ensure recorder "
                 "failed above)",
                 app, stream);
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    zcs_http_hls_segmenter_http_enter(rec);

    if (file && file[0] && strlen(file) >= 5 &&
#if defined(_WIN32)
        _stricmp(file + strlen(file) - 5, ".m3u8") == 0
#else
        strcasecmp(file + strlen(file) - 5, ".m3u8") == 0
#endif
    ) {
        static const char k_hls_empty[] =
            "#EXTM3U\r\n#EXT-X-VERSION:3\r\n#EXT-X-TARGETDURATION:2\r\n#EXT-X-MEDIA-SEQUENCE:0\r\n";

        if (zcs_http_hls_segmenter_serve_m3u8(rec, (char *)hs->send_buf, hs->send_cap, &m3u8_len, 0,
                                              &seg_count) != ZN_OK ||
            m3u8_len == 0 || seg_count <= 0) {
            zn_info("HLS live m3u8 empty: app=%s stream=%s", app, stream);
            zcs_http_response_send_bytes(hs, 200, "application/vnd.apple.mpegurl", k_hls_empty,
                                         sizeof(k_hls_empty) - 1);
            hls_http_done_request(hs, rec);
            return;
        }
        m3u8_len = zcs_http_route_rewrite_live_hls_m3u8((char *)hs->send_buf, hs->send_cap,
                                                        src->app, src->stream);
        zcs_media_event_play(src, "hls");
        {
            char latest[64];

            latest[0] = '\0';
            (void)zcs_http_hls_playlist_latest_segment(zcs_http_hls_segmenter_playlist(rec), latest,
                                                       sizeof(latest), NULL);
            zn_info("HLS live m3u8: app=%s stream=%s len=%u segs=%d latest=%s", app, stream,
                     (unsigned)m3u8_len, seg_count, latest[0] ? latest : "-");
        }
        zcs_http_response_send_bytes(hs, 200, "application/vnd.apple.mpegurl", hs->send_buf,
                                     m3u8_len);
        hls_http_done_request(hs, rec);
        return;
    }

    if (!file || !file[0]) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        hls_http_done_request(hs, rec);
        return;
    }

    {
        zn_buf *seg = NULL;

        zn_info("HLS live segment req: app=%s stream=%s file=%s", app, stream, file);
        if (zcs_http_hls_segmenter_ref_segment(rec, file, &seg, &ts_len, 0) != ZN_OK || !seg ||
            ts_len == 0) {
            zn_warn("HLS live segment 404: app=%s stream=%s file=%s", app, stream, file);
            zcs_http_response_send_error(hs, 404, "Not Found");
            hls_http_done_request(hs, rec);
            return;
        }
        zcs_media_event_play(src, "hls");
        {
            const char *ctype = "video/mp2t";
            size_t flen = strlen(file);
#if defined(_WIN32)
            if (flen >= 4 &&
                (_stricmp(file + flen - 4, ".m4s") == 0 || _stricmp(file + flen - 4, ".mp4") == 0))
#else
            if (flen >= 4 && (strcasecmp(file + flen - 4, ".m4s") == 0 ||
                              strcasecmp(file + flen - 4, ".mp4") == 0))
#endif
                ctype = "video/mp4";
            zn_info("HLS live segment: app=%s stream=%s file=%s len=%u", app, stream, file,
                     (unsigned)ts_len);
            zcs_http_response_send_bytes_buf(hs, 200, ctype, seg);
        }
        zn_buf_unref(seg);
        hls_http_done_request(hs, rec);
    }
}
