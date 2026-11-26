/**
 * @file session_ctrl.c
 * @brief RTSP 控制方法：OPTIONS/PAUSE/GET_PARAMETER/TEARDOWN。
 */
#include "zms/core/stream/hub.h"
#include "session/rtsp/session_internal.h"
#include "zms/core/gop/queue.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void zms_rtsp_session_handle_options(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    (void)msg;
    zms_rtsp_session_send_resp(rs, 200, "OK",
                               "Public: OPTIONS, DESCRIBE, ANNOUNCE, SETUP, PLAY, PAUSE, "
                               "GET_PARAMETER, RECORD, TEARDOWN\r\n",
                               NULL, 0);
}

void zms_rtsp_session_handle_pause(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    (void)msg;
    if (rs && rs->mode == ZMS_RTSP_SESSION_MODE_PLAY) {
        rs->pl.paused = 1;
        if (rs->pl.vod_reader) {
            rs->pl.seek_ms = zms_rtsp_session_vod_play_position_ms(rs);
            if (rs->pl.sender) {
                zms_rtp_play_sender_reset(rs->pl.sender);
            }
            if (rs->tcp) {
                zn_tcp_session_out_discard(rs->tcp);
            }
        }
        if (rs->pl.rtp_muxer) {
            zms_rtp_muxer_pause_play(rs->pl.rtp_muxer);
        }
        zn_info("RTSP #%u PAUSE at media_ms=%llu", rs->session_no,
                 (unsigned long long)rs->pl.seek_ms);
    }
    zms_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
}

void zms_rtsp_session_handle_get_parameter(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    (void)msg;
    if (rs && rs->source && zms_media_source_is_vod(rs->source) &&
        rs->mode == ZMS_RTSP_SESSION_MODE_PLAY) {
        uint64_t dur_ms = zms_rtsp_session_vod_duration_ms(rs);
        uint64_t pos_ms = zms_rtsp_session_vod_play_position_ms(rs);
        char body[128];
        size_t body_len;

        body_len = (size_t)snprintf(body, sizeof(body), "position: %.3f\r\nduration: %.3f\r\n",
                                    pos_ms / 1000.0, dur_ms / 1000.0);
        zms_rtsp_session_send_resp(rs, 200, "OK", "Content-Type: text/parameters\r\n", body,
                                   body_len);
        return;
    }
    zms_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
}

void zms_rtsp_session_handle_teardown(zms_rtsp_session *rs, const zms_rtsp_message *msg)
{
    (void)msg;
    zms_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
    rs->close_pending = 1;
}
