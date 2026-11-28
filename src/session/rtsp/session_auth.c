/**
 * @file session_auth.c
 * @brief RTSP 鉴权配置与请求校验。
 */
#include "zcs/session/rtsp/session_auth.h"
#include "session/rtsp/session_internal.h"
#include "zcs/protocol/rtsp/digest.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_auth_user[64];
static char g_auth_pass[128];
static int g_test_reject_tcp;
static int g_rtp_transport_type = -1;
static int g_low_latency;

void zcs_rtsp_auth_configure(const char *user, const char *pass)
{
    g_auth_user[0] = g_auth_pass[0] = '\0';
    if (user && user[0]) {
        strncpy(g_auth_user, user, sizeof(g_auth_user) - 1);
    }
    if (pass && pass[0]) {
        strncpy(g_auth_pass, pass, sizeof(g_auth_pass) - 1);
    }
}

void zcs_rtsp_set_test_reject_tcp_setup(int enable)
{
    g_test_reject_tcp = enable ? 1 : 0;
}

void zcs_rtsp_set_rtp_transport_type(int type)
{
    g_rtp_transport_type = type;
}

int zcs_rtsp_rtp_transport_type(void)
{
    return g_rtp_transport_type;
}

void zcs_rtsp_set_low_latency(int enable)
{
    g_low_latency = enable ? 1 : 0;
}

int zcs_rtsp_low_latency_enabled(void)
{
    return g_low_latency;
}

int zcs_rtsp_auth_enabled(void)
{
    return g_auth_user[0] && g_auth_pass[0];
}

int zcs_rtsp_test_reject_tcp_setup(void)
{
    return g_test_reject_tcp;
}

int zcs_rtsp_auth_check(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    if (!rs || !msg || !zcs_rtsp_auth_enabled()) {
        return 1;
    }

    if (rs->auth_ok) {
        return 1;
    }

    const char *auth = zcs_rtsp_message_get(msg, "Authorization");
    if (!auth || !auth[0]) {
        snprintf(rs->auth_nonce, sizeof(rs->auth_nonce), "%08x%08x", (unsigned)rand(),
                 (unsigned)rand());
        char extra[256];
        snprintf(extra, sizeof(extra), "WWW-Authenticate: Digest realm=\"ZCS\", nonce=\"%s\"\r\n",
                 rs->auth_nonce);
        zcs_rtsp_session_send_resp(rs, 401, "Unauthorized", extra, NULL, 0);
        zn_info("RTSP #%u 401 auth required method=%d", rs->session_no, (int)msg->method);
        return 0;
    }

    const char *method = "OPTIONS";
    static const char *names[] = {"OPTIONS",  "DESCRIBE", "SETUP",         "PLAY",
                                  "PAUSE",    "TEARDOWN", "GET_PARAMETER", "SET_PARAMETER",
                                  "ANNOUNCE", "RECORD"};
    if (msg->method >= 0 && msg->method < (int)(sizeof(names) / sizeof(names[0]))) {
        method = names[msg->method];
    }

    if (zcs_rtsp_digest_verify(method, zcs_rtsp_message_url(msg), g_auth_user, g_auth_pass, "ZCS", rs->auth_nonce,
                               auth)) {
        rs->auth_ok = 1;
        zn_info("RTSP #%u auth ok user=%s", rs->session_no, g_auth_user);
        return 1;
    }

    zcs_rtsp_session_send_resp(rs, 401, "Unauthorized", NULL, NULL, 0);
    zn_warn("RTSP #%u auth failed", rs->session_no);
    return 0;
}
