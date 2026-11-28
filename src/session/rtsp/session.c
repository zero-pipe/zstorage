/**
 * @file session.c
 * @brief RTSP 会话消息入口：按方法分发到各 handler。
 */
#include "session/rtsp/session_internal.h"
#include "zn/util/log.h"
#include <stdlib.h>

static const zcs_rtsp_method_fn k_rtsp_method_fns[ZCS_RTSP_UNKNOWN] = {
    [ZCS_RTSP_OPTIONS] = zcs_rtsp_session_handle_options,
    [ZCS_RTSP_DESCRIBE] = zcs_rtsp_session_handle_describe,
    [ZCS_RTSP_SETUP] = zcs_rtsp_session_handle_setup,
    [ZCS_RTSP_PLAY] = zcs_rtsp_session_handle_play,
    [ZCS_RTSP_PAUSE] = zcs_rtsp_session_handle_pause,
    [ZCS_RTSP_TEARDOWN] = zcs_rtsp_session_handle_teardown,
    [ZCS_RTSP_GET_PARAMETER] = zcs_rtsp_session_handle_get_parameter,
    [ZCS_RTSP_ANNOUNCE] = zcs_rtsp_session_handle_announce,
    [ZCS_RTSP_RECORD] = zcs_rtsp_session_handle_record,
};

void zcs_rtsp_session_on_message(const zcs_rtsp_message *msg, void *user)
{
    zcs_rtsp_session *rs = (zcs_rtsp_session *)user;
    if (!rs || !msg) {
        return;
    }

    const char *cseq = zcs_rtsp_message_get(msg, "CSeq");
    rs->cseq = cseq ? (unsigned)atoi(cseq) : 0;

    if (zcs_rtsp_message_is_response(msg)) {
        return;
    }

    if (msg->method >= 0 && msg->method < ZCS_RTSP_UNKNOWN && k_rtsp_method_fns[msg->method]) {
        k_rtsp_method_fns[msg->method](rs, msg);
        return;
    }
    zcs_rtsp_session_send_resp(rs, 501, "Not Implemented", NULL, NULL, 0);
}
