/**
 * @file session_describe.c
 * @brief RTSP DESCRIBE：查找媒体源并返回 SDP。
 */
#include "session/rtsp/session_internal.h"
#include "zcs/session/rtsp/session_auth.h"
#include "zcs/core/stream/hub.h"
#include "zn/util/log.h"
#include <string.h>

void zcs_rtsp_session_handle_describe(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    if (!zcs_rtsp_auth_check(rs, msg)) {
        return;
    }
    zcs_rtsp_session_parse_url(zcs_rtsp_message_url(msg), rs->app, rs->stream);
    zcs_rtsp_session_capture_params(rs, zcs_rtsp_message_url(msg));
    zcs_rtsp_url_vhost(zcs_rtsp_message_url(msg), rs->vhost, sizeof(rs->vhost));
    rs->source = zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTSP, rs->vhost, rs->app, rs->stream);
    if (!rs->source || (!zcs_media_source_is_vod(rs->source) &&
                        !zcs_media_source_use_gop_queue_play(rs->source))) {
        zn_warn("RTSP DESCRIBE 404: stream not found app=%s stream=%s url=%s (active_sources=%d)",
                 rs->app, rs->stream, zcs_rtsp_message_url(msg), zcs_media_source_count());
        zcs_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
        return;
    }
    char sdp[4096];
    zcs_rtsp_session_build_sdp(rs, sdp, sizeof(sdp));
    zn_info("RTSP DESCRIBE 200: app=%s stream=%s video=%d audio=%d sdp_bytes=%u", rs->app,
             rs->stream, rs->source->has_video, rs->source->has_audio, (unsigned)strlen(sdp));
    zcs_rtsp_session_send_resp(rs, 200, "OK", "Content-Type: application/sdp\r\n", sdp,
                               strlen(sdp));
}
