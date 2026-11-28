/**
 * @file session_ctrl.c
 * @brief RTSP 控制方法：OPTIONS/PAUSE/GET_PARAMETER/TEARDOWN。
 */
#include "zcs/core/stream/hub.h"
#include "session/rtsp/session_internal.h"
#include "zcs/core/gop/queue.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void zcs_rtsp_session_handle_options(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    (void)msg;
    zcs_rtsp_session_send_resp(rs, 200, "OK",
                               "Public: OPTIONS, DESCRIBE, ANNOUNCE, SETUP, PLAY, PAUSE, "
                               "GET_PARAMETER, RECORD, TEARDOWN\r\n",
                               NULL, 0);
}

void zcs_rtsp_session_handle_pause(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    (void)msg;
    if (rs && rs->mode == ZCS_RTSP_SESSION_MODE_PLAY) {
        rs->pl.paused = 1;
        if (rs->pl.vod_reader) {
            rs->pl.seek_ms = zcs_rtsp_session_vod_play_position_ms(rs);
            if (rs->pl.sender) {
                zcs_rtp_play_sender_reset(rs->pl.sender);
            }
            if (rs->tcp) {
                zn_tcp_session_out_discard(rs->tcp);
            }
        }
        if (rs->pl.rtp_muxer) {
            zcs_rtp_muxer_pause_play(rs->pl.rtp_muxer);
        }
        zn_info("RTSP #%u PAUSE at media_ms=%llu", rs->session_no,
                 (unsigned long long)rs->pl.seek_ms);
    }
    zcs_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
}

void zcs_rtsp_session_handle_get_parameter(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    (void)msg;
    if (rs && rs->source && zcs_media_source_is_vod(rs->source) &&
        rs->mode == ZCS_RTSP_SESSION_MODE_PLAY) {
        uint64_t dur_ms = zcs_rtsp_session_vod_duration_ms(rs);
        uint64_t pos_ms = zcs_rtsp_session_vod_play_position_ms(rs);
        char body[128];
        size_t body_len;

        body_len = (size_t)snprintf(body, sizeof(body), "position: %.3f\r\nduration: %.3f\r\n",
                                    pos_ms / 1000.0, dur_ms / 1000.0);
        zcs_rtsp_session_send_resp(rs, 200, "OK", "Content-Type: text/parameters\r\n", body,
                                   body_len);
        return;
    }
    zcs_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
}

void zcs_rtsp_session_handle_teardown(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    (void)msg;
    zcs_rtsp_session_send_resp(rs, 200, "OK", NULL, NULL, 0);
    rs->close_pending = 1;
}
