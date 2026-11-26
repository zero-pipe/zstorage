/**
 * @file sidecar.c
 * @brief DASH 录制 sidecar 装配。
 */
#include "zms/play/dash/sidecar.h"
#include "zms/play/dash/segmenter.h"
#include "zms/play/hls/sidecar.h"
#include "zms/egress/recorder/egress_segment_recorder.h"
#include "zms/core/stream/hub.h"
#include "zn/util/log.h"

void zms_http_dash_ensure_recorder(zms_media_source *src, zn_poller *poller)
{
    zms_http_dash_segmenter_opts ropts;
    zn_err_t err;
    zn_poller *bind_pol;

    (void)poller;
    if (!src || zms_media_source_is_vod(src)) {
        return;
    }

    zms_http_dash_segmenter_default_opts(&ropts);
    bind_pol = zms_http_hls_main_poller();
    err = zms_segment_recorder_ensure_live(src, ZMS_SEGMENT_REC_DASH, bind_pol, &ropts);
    if (err != ZN_OK) {
        zn_warn("DASH ensure recorder failed: %s/%s err=%d", src->app, src->stream, (int)err);
    }
}
