/**
 * @file session_play.c
 * @brief RTSP PLAY：播放时钟、RTP 输出与 pacing pump。
 */
#include "zcs/play/vod_play_port.h"
#include "session/rtsp/session_internal.h"
#include "zn/platform.h"
#include "zcs/egress/pacing/egress_live_policy.h"
#include "zcs/play/dispatch.h"
#include "zcs/play/play_binding.h"
#include "zcs/core/stream/hub.h"
#include "zcs/media/codec/g711/g711_over_rtp.h"
#include "zcs/media/codec/av1/av1_over_rtmp.h"
#include "zcs/util/thread_pool.h"
#include "zcs/core/stream/buffer.h"
#include "zcs/media/clock.h"
#include "zcs/core/event.h"
#include "zcs/core/gop/queue.h"
#include "zcs/media/wire/rtcp.h"
#include "zcs/protocol/rtsp/parser.h"
#include "zcs/session/rtsp/session_auth.h"
#include "zn/poller/poller.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zcs_rtsp_session_fill_play_ctx(zcs_rtsp_session *rs, zcs_rtp_play_run_ctx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    if (!rs) {
        return;
    }
    ctx->mux = rs->pl.rtp_muxer;
    ctx->egress = rs->pl.egress_pipe;
    ctx->sender = rs->pl.sender;
    ctx->gop_reader = rs->pl.gop_reader;
    ctx->vod_reader = rs->pl.vod_reader;
    ctx->vod_lane = rs->pl.vod_lane;
    ctx->source = rs->source;
    ctx->close_pending = &rs->close_pending;
    ctx->destroy_scheduled = &rs->destroy_scheduled;
    ctx->play_config_pending = &rs->pl.config_pending;
    ctx->play_live_catchup = &rs->pl.live_catchup;
    ctx->play_lag_resync_ms = &rs->pl.lag_resync_ms;
    ctx->session_no = rs->session_no;
    ctx->low_latency = rs->low_latency;
}

zcs_rtsp_play_rtp_info_args zcs_rtsp_session_rtp_info_args(zcs_rtsp_session *rs, uint32_t anchor_ms,
                                                           int vod_linear)
{
    zcs_rtsp_play_rtp_info_args args;

    memset(&args, 0, sizeof(args));
    if (!rs) {
        return args;
    }
    args.host = rs->peer_ip[0] ? rs->peer_ip : "127.0.0.1";
    args.app = rs->app;
    args.stream = rs->stream;
    args.source = rs->source;
    args.mux = rs->pl.rtp_muxer;
    args.video_clock_hz = rs->pb.video_clock_hz;
    args.audio_clock_hz = rs->pb.audio_clock_hz;
    args.audio_rate = rs->pb.audio_rate;
    args.anchor_ms = anchor_ms;
    args.vod_linear_rtp = vod_linear;
    return args;
}

void zcs_rtsp_session_egress_close(zcs_rtsp_session *rs)
{
    zcs_play_binding bind;

    if (!rs) {
        return;
    }
    memset(&bind, 0, sizeof(bind));
    bind.source = &rs->source;
    bind.play = &rs->pl.src;
    bind.gop_reader = &rs->pl.gop_reader;
    bind.vod_reader = &rs->pl.vod_reader;
    bind.vod_lane = &rs->pl.vod_lane;
    bind.reader_attached = &rs->pl.reader_attached;
    bind.play_start_ms = &rs->play_start_ms;
    bind.player = "rtsp";
    zcs_play_binding_close_readers(&bind);
}

static void zcs_rtsp_session_play_pump(zcs_rtsp_session *rs, int budget, int flush)
{
    zcs_rtp_play_run_ctx ctx;

    if (!rs) {
        return;
    }
    zcs_rtsp_session_fill_play_ctx(rs, &ctx);
    (void)zcs_rtp_play_run_pump(&ctx, budget, flush);
    zcs_rtsp_session_play_try_rtcp_boot(rs);
}

void zcs_rtsp_session_play_tick(zcs_rtsp_session *rs)
{
    zcs_rtp_play_run_ctx ctx;
    int budget;

    if (!rs || rs->close_pending || rs->destroy_scheduled || rs->pl.paused ||
        rs->mode != ZCS_RTSP_SESSION_MODE_PLAY || !rs->pl.rtp_muxer) {
        return;
    }
    zcs_rtsp_session_fill_play_ctx(rs, &ctx);
    budget = zcs_rtp_play_frame_budget(&ctx);
    zcs_rtsp_session_play_pump(rs, budget, ZCS_RTP_PLAY_RTP_FLUSH);
    if (rs->close_pending && !rs->destroy_scheduled) {
        zcs_rtsp_session_schedule_destroy(rs, rs->tcp);
    }
}

void zcs_rtsp_session_play_kick(zcs_rtsp_session *rs)
{
    zcs_rtp_play_run_ctx ctx;
    int budget;

    if (!rs || rs->close_pending || rs->destroy_scheduled || rs->pl.paused ||
        rs->mode != ZCS_RTSP_SESSION_MODE_PLAY || !rs->pl.rtp_muxer) {
        return;
    }
    if (!rs->pl.boot_sent) {
        if (rs->source && zcs_media_source_is_vod(rs->source)) {
            (void)zcs_rtp_play_bootstrap_vod(rs->pl.rtp_muxer, rs->source, rs->pl.vod_reader,
                                             rs->session_no, rs->pl.vod_lane, 0);
        } else {
            (void)zcs_rtp_play_bootstrap_live(rs->pl.rtp_muxer, rs->source, rs->pl.gop_reader,
                                              rs->session_no);
        }
        rs->pl.boot_sent = 1;
        rs->pl.config_pending = 0;
        if (rs->pl.vod_reader) {
            rs->pl.live_catchup = 1;
        } else {
            rs->pl.live_catchup = 0;
        }
        if (rs->pl.rtp_muxer && rs->pl.vod_reader && !zcs_rtp_muxer_catchup_on(rs->pl.rtp_muxer)) {
            zcs_rtp_muxer_set_catchup_budget(rs->pl.rtp_muxer, 1,
                                             (int)ZCS_RTP_VOD_CATCHUP_FRAMES_START);
        }
    }
    zcs_rtsp_session_fill_play_ctx(rs, &ctx);
    budget = zcs_rtp_play_kick_budget(&ctx);
    zcs_rtsp_session_play_pump(rs, budget, ZCS_RTP_PLAY_RTP_FLUSH);
    if (rs->tcp) {
        zn_tcp_session_flush(rs->tcp);
    }
}

static void on_mux_rtp(zcs_rtp_mux_track track, const uint8_t *rtp, size_t len, void *user)
{
    zcs_rtsp_session *rs = (zcs_rtsp_session *)user;
    const zcs_rtp_muxer_stats *st;

    if (!rs || rs->close_pending || rs->destroy_scheduled ||
        rs->mode != ZCS_RTSP_SESSION_MODE_PLAY || !rtp || len == 0) {
        return;
    }
    if (rs->source) {
        zcs_media_stats_on_egress(rs->source, len);
    }
    st = zcs_rtp_muxer_get_stats(rs->pl.rtp_muxer);
    if (track == ZCS_RTP_MUX_TRACK_VIDEO) {
        if (st && st->video_pkt_count == 1) {
            zn_info("RTSP #%u first RTP: ch=%u bytes=%u transport=%s", rs->session_no,
                     (unsigned)rs->video_rtp_ch, (unsigned)len,
                     rs->rtp_mode == ZCS_RTSP_RTP_UDP ? "udp" : "tcp");
        }
        if (st && rs->pl.vod_reader && rs->source && zcs_media_source_is_vod(rs->source)) {
            uint32_t vhz = rs->pb.video_clock_hz > 0 ? rs->pb.video_clock_hz : 90000u;
            rs->pl.seek_ms = zcs_rtp_clock_to_ms(st->video_last_rtp_ts, vhz);
        }
        zcs_rtp_play_sender_submit(rs->pl.sender, 1, rs->video_rtp_ch, rtp, len);
    } else {
        if (st && st->audio_pkt_count == 1) {
            zn_info("RTSP #%u first audio RTP: ch=%u bytes=%u transport=%s", rs->session_no,
                     (unsigned)rs->audio_rtp_ch, (unsigned)len,
                     rs->rtp_mode == ZCS_RTSP_RTP_UDP ? "udp" : "tcp");
        }
        zcs_rtp_play_sender_submit(rs->pl.sender, 1, rs->audio_rtp_ch, rtp, len);
    }
    zcs_rtsp_session_play_try_rtcp_boot(rs);
}

void zcs_rtsp_session_play_mux_destroy(zcs_rtsp_session *rs)
{
    zcs_rtp_play_mux_handle opened;

    if (!rs) {
        return;
    }
    opened.egress = rs->pl.egress_pipe;
    opened.mux = rs->pl.rtp_muxer;
    zcs_rtp_play_mux_destroy(&opened);
    rs->pl.egress_pipe = NULL;
    rs->pl.rtp_muxer = NULL;
}

void zcs_rtsp_session_play_mux_create(zcs_rtsp_session *rs)
{
    zcs_rtp_muxer_opts opts;
    zcs_rtp_play_mux_opts ocfg;
    zcs_rtp_play_mux_handle opened;

    if (!rs) {
        return;
    }
    zcs_rtsp_session_play_mux_destroy(rs);
    memset(&opts, 0, sizeof(opts));
    opts.video_clock_hz = rs->pb.video_clock_hz > 0 ? rs->pb.video_clock_hz : 90000u;
    opts.audio_clock_hz = rs->pb.audio_clock_hz;
    opts.audio_rate = rs->pb.audio_rate;
    opts.audio_codec = rs->pb.audio_codec;
    opts.video_pt = 96;
    opts.audio_pt = (rs->pb.audio_codec == ZCS_CODEC_G711A || rs->pb.audio_codec == ZCS_CODEC_G711U)
                        ? zcs_g711_over_rtp_default_pt(rs->pb.audio_codec)
                        : 97;
    opts.video_ssrc = rs->pl.video_rtp_ssrc;
    opts.audio_ssrc = rs->pl.audio_rtp_ssrc;
    opts.video_seq = 1;
    opts.audio_seq = 1;
    if (rs->source && rs->pb.audio_codec == ZCS_CODEC_AAC) {
        size_t acfg_len = 0;
        const uint8_t *acfg = zcs_media_source_audio_config(rs->source, &acfg_len);

        if (acfg && acfg_len > 2) {
            opts.audio_extra = acfg + 2;
            opts.audio_extra_len = acfg_len - 2;
        }
    }
    if (rs->source && rs->source->has_video && rs->source->video.codec == ZCS_CODEC_AV1) {
        size_t vcfg_len = 0;
        const uint8_t *vcfg = zcs_media_source_video_config(rs->source, &vcfg_len);
        const uint8_t *av1c = NULL;
        size_t av1c_len = 0;

        if (vcfg && zcs_av1_over_rtmp_config_extradata(vcfg, vcfg_len, &av1c, &av1c_len) && av1c &&
            av1c_len > 0) {
            opts.video_extra = av1c;
            opts.video_extra_len = av1c_len;
        }
    }
    memset(&ocfg, 0, sizeof(ocfg));
    ocfg.mux_opts = opts;
    ocfg.reader = &rs->pl.src;
    ocfg.on_rtp = on_mux_rtp;
    ocfg.user = rs;
    if (zcs_rtp_play_mux_create(&opened, &ocfg) == ZN_OK) {
        rs->pl.egress_pipe = opened.egress;
        rs->pl.rtp_muxer = opened.mux;
        zcs_rtp_muxer_arm_play(rs->pl.rtp_muxer);
    }
}

/* 播放鉴权：上游签发的 token，本地同步验签；拒绝时回 403。 */
static int rtsp_play_authorize(zcs_rtsp_session *rs)
{
    zcs_auth_request req;
    zcs_auth_decision dec;
    char ip[64];
    unsigned port = 0;

    zcs_auth_fill_peer(rs->tcp, ip, sizeof(ip), &port);
    memset(&req, 0, sizeof(req));
    req.action = ZCS_AUTH_ACTION_PLAY;
    req.schema = ZCS_SCHEMA_RTSP;
    req.vhost = rs->vhost;
    req.app = rs->app;
    req.stream = rs->stream;
    req.params = rs->params[0] ? rs->params : NULL;
    req.ip = ip[0] ? ip : NULL;
    req.port = port;
    if (zcs_auth_check(&req, &dec)) {
        return 1;
    }
    zn_warn("RTSP PLAY denied: app=%s stream=%s", rs->app, rs->stream);
    zcs_rtsp_session_send_resp(rs, 403, "Forbidden", NULL, NULL, 0);
    return 0;
}

void zcs_rtsp_session_handle_play(zcs_rtsp_session *rs, const zcs_rtsp_message *msg)
{
    if (!rs || !msg) {
        return;
    }
    if (!zcs_rtsp_auth_check(rs, msg)) {
        return;
    }
    zcs_rtsp_session_parse_url(zcs_rtsp_message_url(msg), rs->app, rs->stream);
    zcs_rtsp_session_capture_params(rs, zcs_rtsp_message_url(msg));
    if (!rs->source || (!rs->source->gop_queue && !rs->source->vod_buffer)) {
        rs->source = zcs_media_source_find_for_play_ex(ZCS_SCHEMA_RTSP, rs->vhost, rs->app, rs->stream);
    }
    if (rs->source && zcs_media_source_is_vod(rs->source)) {
        int is_replay = rs->pl.reader_attached;
        int was_paused = rs->pl.paused;
        int range_parsed = 0;
        int range_now = 0;
        int did_seek = 0;
        uint64_t seek_ms = 0;
        double scale;
        const char *range_req = zcs_rtsp_message_get(msg, "Range");

        scale = zcs_rtsp_parse_play_scale(zcs_rtsp_message_get(msg, "Scale"), rs->pl.scale);
        rs->pl.scale = scale;
        rs->pl.rtcp_tick = 0;
        rs->pl.live_catchup = 0;
        rs->pl.lag_resync_ms = 0;
        zcs_rtsp_session_load_audio_params(rs);
        if (rs->source->has_audio && !rs->pl.audio_setup) {
            zn_warn("RTSP PLAY: audio track not SETUP 拒绝ffplay needs SETUP trackID=1 for sound");
        }
        if (!rtsp_play_authorize(rs)) {
            return;
        }
        zcs_rtsp_splitter_enable_rtp(rs->splitter, 1);
        rs->pl.paused = 0;
        rs->mode = ZCS_RTSP_SESSION_MODE_PLAY;

        if (range_req) {
            range_parsed = zcs_rtsp_parse_range_npt_ms(range_req, &seek_ms, &range_now);
        }
        if (!is_replay) {
            if (!range_parsed || range_now) {
                seek_ms = 0;
            }
            did_seek = 1;
        } else if (range_parsed && !range_now) {
            did_seek = 1;
        } else {
            seek_ms = rs->pl.seek_ms;
        }

        if (did_seek) {
            zcs_session_play_opts pcfg;
            uint64_t req_ms = seek_ms;

            memset(&pcfg, 0, sizeof(pcfg));
            pcfg.player = ZCS_SESSION_RTSP;
            pcfg.seek_ms = seek_ms;
            if (is_replay && req_ms == rs->pl.seek_ms) {
                zn_info("RTSP #%u seek refresh: same pos %llu ms (skip lane re-seek)",
                         rs->session_no, (unsigned long long)req_ms);
            } else if (!rs->pl.vod_lane && zcs_vod_thread_pool_enabled()) {
                zcs_rtsp_vod_play_ctx *job = (zcs_rtsp_vod_play_ctx *)calloc(1, sizeof(*job));
                if (job) {
                    job->rs = rs;
                    job->src = rs->source;
                    job->seek_ms = seek_ms;
                    job->is_replay = is_replay;
                    job->was_paused = was_paused;
                    job->did_seek = did_seek;
                    job->range_parsed = range_parsed;
                    job->range_now = range_now;
                    job->scale = scale;
                    if (range_req && range_req[0]) {
                        strncpy(job->range_req, range_req, sizeof(job->range_req) - 1);
                    }
                    if (zcs_rtsp_session_vod_open_try_async(job)) {
                        return;
                    }
                    free(job);
                }
                if (zcs_session_attach_play(ZCS_SESSION_RTSP, rs, rs->source, &pcfg) != ZN_OK) {
                    zn_warn("RTSP PLAY 404: vod attach failed app=%s stream=%s", rs->app,
                             rs->stream);
                    zcs_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
                    return;
                }
            } else if (zcs_session_attach_play(ZCS_SESSION_RTSP, rs, rs->source, &pcfg) != ZN_OK) {
                zn_warn("RTSP PLAY 404: vod attach failed app=%s stream=%s", rs->app, rs->stream);
                zcs_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
                return;
            }
            seek_ms = rs->pl.seek_ms;
        } else if (!rs->pl.vod_lane || !rs->pl.vod_reader) {
            zn_warn("RTSP PLAY 404: vod lane missing on replay app=%s stream=%s", rs->app,
                     rs->stream);
            zcs_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
            return;
        } else {
            seek_ms = rs->pl.seek_ms;
        }

        zcs_rtsp_session_vod_play_continue(rs, is_replay, was_paused, did_seek, seek_ms, scale,
                                           range_req, range_parsed, range_now);
    } else if (rs->source && zcs_media_source_use_gop_queue_play(rs->source)) {
        zcs_session_play_opts pcfg;

        memset(&pcfg, 0, sizeof(pcfg));
        pcfg.player = ZCS_SESSION_RTSP;
        zcs_session_detach_play(ZCS_SESSION_RTSP, rs);
        if (zcs_session_attach_play(ZCS_SESSION_RTSP, rs, rs->source, &pcfg) != ZN_OK) {
            rs->pl.gop_reader = NULL;
        }
        rs->pl.video_rtp_ssrc = 0x12345678;
        rs->pl.audio_rtp_ssrc = 0x87654321;
        rs->pl.rtcp_tick = 0;
        rs->pl.boot_sent = 0;
        rs->pl.rtcp_boot_sent = 0;
        rs->pl.live_catchup = 0;
        rs->pl.lag_resync_ms = 0;
        zcs_rtp_play_sender_reset(rs->pl.sender);
        zcs_rtsp_session_load_audio_params(rs);
        if (rs->source->has_audio && !rs->pl.audio_setup) {
            zn_warn("RTSP PLAY: audio track not SETUP 拒绝ffplay needs SETUP trackID=1 for sound");
        }
        if (!rtsp_play_authorize(rs)) {
            return;
        }
        zcs_rtsp_splitter_enable_rtp(rs->splitter, 1);
        rs->mode = ZCS_RTSP_SESSION_MODE_PLAY;
        zcs_rtsp_session_play_mux_create(rs);
        {
            uint32_t anchor_ms = 0;
            zcs_gop_slot at;

            (void)zcs_rtp_play_bootstrap_live(rs->pl.rtp_muxer, rs->source, rs->pl.gop_reader,
                                              rs->session_no);
            rs->pl.boot_sent = 1;
            rs->pl.config_pending = 0;
            rs->pl.live_catchup = 0;
            if (rs->pl.gop_reader && zcs_gop_reader_slot_at_read(rs->pl.gop_reader, &at)) {
                anchor_ms = at.dts_ms;
            }
            zn_info("RTSP #%u PLAY 200: app=%s stream=%s video=%d audio=%d es=%d reader=%s "
                     "transport=%s anchor_ms=%u",
                     rs->session_no, rs->app, rs->stream, rs->source->has_video,
                     rs->source->has_audio, rs->pl.gop_reader ? 1 : 0,
                     rs->pl.gop_reader ? "gop_queue" : "none",
                     rs->rtp_mode == ZCS_RTSP_RTP_UDP ? "udp" : "tcp", (unsigned)anchor_ms);
            {
                char extra[ZCS_RTSP_PLAY_EXTRA_MAX];
                char rtp_info[ZCS_RTSP_RTP_INFO_MAX];

                {
                    zcs_rtsp_play_rtp_info_args rtp_args =
                        zcs_rtsp_session_rtp_info_args(rs, anchor_ms, 0);
                    zcs_rtsp_play_format_rtp_info(&rtp_args, rtp_info, sizeof(rtp_info));
                }
                zcs_rtsp_play_format_live_play_200(extra, sizeof(extra), rs->session_id, rtp_info);
                zcs_rtsp_session_send_resp(rs, 200, "OK", extra, NULL, 0);
            }
        }
        rs->pl.config_pending = 0;
        if (rs->source && !rs->pl.reader_attached) {
            zcs_media_source_reader_add(rs->source);
            rs->play_start_ms = zn_monotonic_ms();
            zcs_media_event_play(rs->source, "rtsp");
            rs->pl.reader_attached = 1;
        }
        zcs_rtsp_session_play_kick(rs);
        if (rs->rtp_mode == ZCS_RTSP_RTP_UDP && rs->pl.sender) {
            zcs_rtp_play_sender_flush(rs->pl.sender, (int)ZCS_RTP_PLAY_RTP_FLUSH, 0);
        }
    } else {
        zn_warn("RTSP PLAY 404: stream not found app=%s stream=%s url=%s", rs->app, rs->stream,
                 zcs_rtsp_message_url(msg));
        zcs_rtsp_session_send_resp(rs, 404, "Not Found", NULL, NULL, 0);
    }
}
