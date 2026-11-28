/**
 * @file dash_handler.c
 * @brief 直播 DASH 分片/清单服务。
 */
#include "zcs/play/http_port.h"
#include "session/http/session_internal.h"
#include "zcs/core/codec_filter.h"
#include "zcs/core/event.h"
#include "zcs/egress/recorder/egress_segment_recorder.h"
#include "zn/util/log.h"
#include <string.h>

static void dash_http_done(zcs_http_dash_segmenter *rec)
{
    zcs_http_dash_segmenter_http_leave(rec, NULL);
}

static void dash_http_done_request(zcs_http_session *hs, zcs_http_dash_segmenter *rec)
{
    (void)hs;
    if (rec) {
        dash_http_done(rec);
    }
}

void zcs_http_live_dash_handle(zcs_http_session *hs, zcs_media_source *src, const char *app,
                              const char *stream, const char *file)
{
    zcs_http_dash_segmenter *rec;
    zcs_http_dash_playlist *maker;
    size_t mpd_len = 0;
    size_t seg_len = 0;

    if (!hs || !src || !src->gop_queue) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    if (zcs_session_capability_check_source(ZCS_PROTO_CAP_DASH_PLAY, src) != ZN_OK) {
        zcs_session_capability_log_reject("dash", src, ZCS_PROTO_CAP_DASH_PLAY);
        zcs_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }

    zcs_http_dash_ensure_recorder(src, zcs_http_hls_main_poller());
    rec = (zcs_http_dash_segmenter *)zcs_media_source_segment_rec_get(src, ZCS_SEGMENT_REC_DASH);
    if (!rec) {
        zn_warn("DASH live 404: recorder unavailable app=%s stream=%s (see DASH ensure recorder "
                 "failed above)",
                 app, stream);
        zcs_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    zcs_http_dash_segmenter_http_enter(rec);
    maker = zcs_http_dash_segmenter_playlist(rec);
    if (!maker) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        dash_http_done_request(hs, rec);
        return;
    }

    if (file && strstr(file, ".mpd")) {
        static const char k_dash_empty[] =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
            "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"dynamic\" "
            "minimumUpdatePeriod=\"PT2S\" profiles=\"urn:mpeg:dash:profile:isoff-live:2011\">\r\n"
            "  <Period start=\"PT0S\" id=\"dash\"/>\r\n"
            "</MPD>\r\n";

        {
            int has_media = 0;

            if (zcs_http_dash_segmenter_serve_mpd(rec, (char *)hs->send_buf, hs->send_cap, &mpd_len,
                                                  0, &has_media) != ZN_OK ||
                mpd_len == 0 || !has_media) {
                zn_info("DASH live mpd waiting for media segment: app=%s stream=%s", app, stream);
                zcs_http_response_send_bytes(hs, 200, "application/dash+xml", k_dash_empty,
                                             sizeof(k_dash_empty) - 1);
                dash_http_done_request(hs, rec);
                return;
            }
        }
        zcs_media_event_play(src, "dash");
        zn_info("DASH live mpd: app=%s stream=%s len=%u v+a_segs=%d", app, stream,
                 (unsigned)mpd_len, zcs_http_dash_playlist_segment_count(maker));
        zcs_http_response_send_bytes(hs, 200, "application/dash+xml", hs->send_buf, mpd_len);
        dash_http_done_request(hs, rec);
        return;
    }

    if (!file || !file[0]) {
        zcs_http_response_send_error(hs, 404, "Not Found");
        dash_http_done_request(hs, rec);
        return;
    }
    {
        zn_buf *seg = NULL;

        zn_info("DASH live segment req: app=%s stream=%s file=%s", app, stream, file);
        if (zcs_http_dash_segmenter_ref_segment(rec, file, &seg, &seg_len, 0) != ZN_OK || !seg ||
            seg_len == 0) {
            zn_warn("DASH live segment 404: app=%s stream=%s file=%s", app, stream, file);
            zcs_http_response_send_error(hs, 404, "Not Found");
            dash_http_done_request(hs, rec);
            return;
        }
        zn_info("DASH live segment: app=%s stream=%s file=%s len=%u", app, stream, file,
                 (unsigned)seg_len);
        if (strstr(file, ".m4a")) {
            zcs_http_response_send_bytes_buf(hs, 200, "audio/mp4", seg);
        } else {
            zcs_http_response_send_bytes_buf(hs, 200, "video/mp4", seg);
        }
        zn_buf_unref(seg);
        dash_http_done_request(hs, rec);
    }
}
