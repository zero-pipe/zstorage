/**
 * @file session_describe.c
 * @brief RTSP DESCRIBE：查找媒体源并返回 SDP。
 */
#include "session/rtsp/session_internal.h"
#include "zms/session/rtsp/session_auth.h"
#include "zms/core/stream/hub.h"
#include "zn/util/log.h"
#include <string.h>

void zms_rtsp_session_handle_describe(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    if (!zms_rtsp_auth_check(rs, msg)) {
        return;
    }
    zms_rtsp_session_parse_url(zms_rtsp_message_url(msg), rs->app, rs->stream);
    zms_rtsp_session_capture_params(rs, zms_rtsp_message_url(msg));
    zms_rtsp_url_vhost(zms_rtsp_message_url(msg), rs->vhost, sizeof(rs->vhost));
    rs->source = zms_media_source_find_for_play_ex(ZMS_SCHEMA_RTSP, rs->vhost, rs->app, rs->stream);
    if (!rs->source || (!zms_media_source_is_vod(rs->source) &&
                        !zms_media_source_use_gop_queue_play(rs->source))) {
        zn_warn("RTSP DESCRIBE 404: stream not found app=%s stream=%s url=%s (active_sources=%d)",
                 rs->app, rs->stream, zms_rtsp_message_url(msg), zms_media_source_count());
        zms_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
        return;
    }
    char sdp[4096];
    zms_rtsp_session_build_sdp(rs, sdp, sizeof(sdp));
    zn_info("RTSP DESCRIBE 200: app=%s stream=%s video=%d audio=%d sdp_bytes=%u", rs->app,
             rs->stream, rs->source->has_video, rs->source->has_audio, (unsigned)strlen(sdp));
    zms_rtsp_session_send_resp(rs, 200, "OK", "Content-Type: application/sdp\r\n", sdp,
                               strlen(sdp));
}
