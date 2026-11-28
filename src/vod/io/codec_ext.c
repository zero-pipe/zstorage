/**
 * @file codec_ext.c
 * @brief VOD 容器扩展 codec 注册。
 */
#include "zcs/vod/io/codec_ext.h"
#include <string.h>

#define ZCS_VOD_CODEC_OPS_MAX 8

static const zcs_vod_codec_ops *g_codec_ops[ZCS_VOD_CODEC_OPS_MAX];
static int g_codec_ops_count;
static int g_vod_codec_ext_builtins_registered;

void zcs_vod_codec_ext_register(const zcs_vod_codec_ops *ops)
{
    int i;

    if (!ops || !ops->name || !ops->name[0]) {
        return;
    }
    for (i = 0; i < g_codec_ops_count; ++i) {
        if (g_codec_ops[i] && strcmp(g_codec_ops[i]->name, ops->name) == 0) {
            g_codec_ops[i] = ops;
            return;
        }
    }
    if (g_codec_ops_count >= ZCS_VOD_CODEC_OPS_MAX) {
        return;
    }
    g_codec_ops[g_codec_ops_count++] = ops;
}

static int hevc_probe_mp4_stub(const char *file_path)
{
    (void)file_path;
    return 0;
}

static const zcs_vod_codec_ops k_hevc_ops = {
    .name = ZCS_VOD_CODEC_HEVC,
    .probe_mp4 = hevc_probe_mp4_stub,
    .lane_prepare = NULL,
    .lane_destroy = NULL,
};

void zcs_vod_codec_ext_register_all(void)
{
    if (g_vod_codec_ext_builtins_registered) {
        return;
    }
    g_vod_codec_ext_builtins_registered = 1;
    zcs_vod_codec_ext_register(&k_hevc_ops);
}
