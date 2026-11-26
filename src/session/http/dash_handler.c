/**
 * @file dash_handler.c
 * @brief 直播 DASH 分片/清单服务。
 */
#include "zms/play/http_port.h"
#include "session/http/session_internal.h"
#include "zms/core/codec_filter.h"
#include "zms/core/event.h"
#include "zms/egress/recorder/egress_segment_recorder.h"
#include "zn/util/log.h"
#include <string.h>

static void dash_http_done(zms_http_dash_segmenter *rec)
{
    zms_http_dash_segmenter_http_leave(rec, NULL);
}

static void dash_http_done_request(zms_http_session *hs, zms_http_dash_segmenter *rec)
{
    (void)hs;
    if (rec) {
        dash_http_done(rec);
    }
}

void zms_http_live_dash_handle(zms_http_session *hs, zms_media_source *src, const char *app,
                              const char *stream, const char *file)
{
    zms_http_dash_segmenter *rec;
    zms_http_dash_playlist *maker;
    size_t mpd_len = 0;
    size_t seg_len = 0;

    if (!hs || !src || !src->gop_queue) {
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }

    if (zms_session_capability_check_source(ZMS_PROTO_CAP_DASH_PLAY, src) != ZN_OK) {
        zms_session_capability_log_reject("dash", src, ZMS_PROTO_CAP_DASH_PLAY);
        zms_http_response_send_error(hs, 406, "Not Acceptable");
        return;
    }

    zms_http_dash_ensure_recorder(src, zms_http_hls_main_poller());
    rec = (zms_http_dash_segmenter *)zms_media_source_segment_rec_get(src, ZMS_SEGMENT_REC_DASH);
    if (!rec) {
        zn_warn("DASH live 404: recorder unavailable app=%s stream=%s (see DASH ensure recorder "
                 "failed above)",
                 app, stream);
        zms_http_response_send_error(hs, 404, "Not Found");
        return;
    }
    zms_http_dash_segmenter_http_enter(rec);
    maker = zms_http_dash_segmenter_playlist(rec);
    if (!maker) {
        zms_http_response_send_error(hs, 404, "Not Found");
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

            if (zms_http_dash_segmenter_serve_mpd(rec, (char *)hs->send_buf, hs->send_cap, &mpd_len,
                                                  0, &has_media) != ZN_OK ||
                mpd_len == 0 || !has_media) {
                zn_info("DASH live mpd waiting for media segment: app=%s stream=%s", app, stream);
                zms_http_response_send_bytes(hs, 200, "application/dash+xml", k_dash_empty,
                                             sizeof(k_dash_empty) - 1);
                dash_http_done_request(hs, rec);
                return;
            }
        }
        zms_media_event_play(src, "dash");
        zn_info("DASH live mpd: app=%s stream=%s len=%u v+a_segs=%d", app, stream,
                 (unsigned)mpd_len, zms_http_dash_playlist_segment_count(maker));
        zms_http_response_send_bytes(hs, 200, "application/dash+xml", hs->send_buf, mpd_len);
        dash_http_done_request(hs, rec);
        return;
    }

    if (!file || !file[0]) {
        zms_http_response_send_error(hs, 404, "Not Found");
        dash_http_done_request(hs, rec);
        return;
    }
    {
        zn_buf *seg = NULL;

        zn_info("DASH live segment req: app=%s stream=%s file=%s", app, stream, file);
        if (zms_http_dash_segmenter_ref_segment(rec, file, &seg, &seg_len, 0) != ZN_OK || !seg ||
            seg_len == 0) {
            zn_warn("DASH live segment 404: app=%s stream=%s file=%s", app, stream, file);
            zms_http_response_send_error(hs, 404, "Not Found");
            dash_http_done_request(hs, rec);
            return;
        }
        zn_info("DASH live segment: app=%s stream=%s file=%s len=%u", app, stream, file,
                 (unsigned)seg_len);
        if (strstr(file, ".m4a")) {
            zms_http_response_send_bytes_buf(hs, 200, "audio/mp4", seg);
        } else {
            zms_http_response_send_bytes_buf(hs, 200, "video/mp4", seg);
        }
        zn_buf_unref(seg);
        dash_http_done_request(hs, rec);
    }
}
