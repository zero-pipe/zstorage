/**
 * @file protocol_opts.c
 * @brief 各协议入站默认选项。
 */
#include "zcs/publish/ingest/common/protocol_opts.h"
#include <string.h>

static zcs_protocol_opts g_protocol_defaults;
static int g_protocol_defaults_set;

void zcs_protocol_opts_set_defaults(const zcs_protocol_opts *opts)
{
    if (!opts) {
        return;
    }
    g_protocol_defaults = *opts;
    g_protocol_defaults_set = 1;
}

void zcs_protocol_opts_default(zcs_protocol_opts *opts)
{
    if (!opts) {
        return;
    }
    if (g_protocol_defaults_set) {
        *opts = g_protocol_defaults;
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->enable_rtmp = 1;
    opts->enable_rtsp = 1;
    opts->enable_srt = 1;
    opts->enable_hls = 1;
    opts->enable_audio = 1;
    opts->add_mute_audio = 1;
    opts->continue_push_ms = 15000;
    opts->modify_stamp = 0;
}
