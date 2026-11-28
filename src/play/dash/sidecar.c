/**
 * @file sidecar.c
 * @brief DASH 录制 sidecar 装配。
 */
#include "zcs/play/dash/sidecar.h"
#include "zcs/play/dash/segmenter.h"
#include "zcs/play/hls/sidecar.h"
#include "zcs/egress/recorder/egress_segment_recorder.h"
#include "zcs/core/stream/hub.h"
#include "zn/util/log.h"

void zcs_http_dash_ensure_recorder(zcs_media_source *src, zn_poller *poller)
{
    zcs_http_dash_segmenter_opts ropts;
    zn_err_t err;
    zn_poller *bind_pol;

    (void)poller;
    if (!src || zcs_media_source_is_vod(src)) {
        return;
    }

    zcs_http_dash_segmenter_default_opts(&ropts);
    bind_pol = zcs_http_hls_main_poller();
    err = zcs_segment_recorder_ensure_live(src, ZCS_SEGMENT_REC_DASH, bind_pol, &ropts);
    if (err != ZN_OK) {
        zn_warn("DASH ensure recorder failed: %s/%s err=%d", src->app, src->stream, (int)err);
    }
}
