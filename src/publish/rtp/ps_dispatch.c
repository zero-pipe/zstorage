/**
 * @file ps_dispatch.c
 * @brief RTP-PS 入站分发注册。
 */
#include "zcs/play/dispatch.h"

static const zcs_session_dispatch_ops k_rtp_ps_dispatch = {
    .name = ZCS_SESSION_RTP_PS,
    .on_play_live = NULL,
    .on_play_vod = NULL,
    .on_publish = NULL,
    .on_teardown = NULL,
};

void zcs_rtp_ps_register(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
    zcs_session_dispatch_register(&k_rtp_ps_dispatch);
}
