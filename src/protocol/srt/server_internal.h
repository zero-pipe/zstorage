/**
 * @file server_internal.h
 * @brief SRT 会话/服务内部。
 */
#ifndef ZCS_SRC_PROTOCOL_SRT_SERVER_INTERNAL_H
#define ZCS_SRC_PROTOCOL_SRT_SERVER_INTERNAL_H

#include "server_struct.h"
#include "zcs/media/container/demux_pipeline.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/core/auth.h"
#include "zcs/core/stream/hub.h"
#include "zn/poller/poller.h"
#include <stdint.h>

/* Play muxer：此处不完整（LoD）；srt_play_session.c / teardown .c 需 include mpegts_egress.h。 */
typedef struct zcs_mpegts_egress zcs_mpegts_egress;

#ifdef ZCS_HAVE_SRT
#include <srt/srt.h>
#else
typedef int SRTSOCKET;
#endif

typedef enum zcs_srt_session_mode {
    ZCS_SRT_SESSION_MODE_PUBLISH = 0,
    ZCS_SRT_SESSION_MODE_PLAY = 1,
} zcs_srt_session_mode;

struct zcs_srt_session {
    zcs_srt_server *server;
    struct zcs_srt_session *next;
    unsigned session_no;
    SRTSOCKET sock;
    zn_poller *poller;
    volatile int stopping;
    volatile int destroy_scheduled;
    zcs_srt_session_mode mode;
    zcs_live_ingest *ingress;
    zcs_media_source *source;
    zcs_demux_pipeline *publish_pipeline;
    zcs_egress_source play;
    zcs_mpegts_egress *live_muxer;
    int reader_attached;
    uint64_t
        play_start_ms; /**< 播放开始 zn_monotonic_ms()；供 on_play_stop 上报 duration_ms */
    int logged_recv;
    int logged_send;
    int logged_recv_stat;
    int logged_recv_stall;
    uint64_t recv_bytes;
    uint64_t send_bytes;
    uint64_t last_recv_bytes;
    unsigned recv_calls;
    unsigned stall_polls;
    char vhost[ZCS_VHOST_MAX];
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    /** streamid 里的查询串（鉴权 token 等）。 */
    char params[ZCS_AUTH_PARAMS_MAX];
    /** 推流收包缓冲（堆，ZCS_SRT_RECV_BUF 字节）。 */
    uint8_t *recv_buf;
};

void zcs_srt_session_publisher_kick(void *ctx, int force);
void zcs_srt_session_schedule_destroy(zcs_srt_session *sess);
void zcs_srt_session_destroy_now(zcs_srt_session *sess);
void zcs_srt_apply_session_opts(SRTSOCKET sock);

int zcs_srt_session_begin_publish(zcs_srt_session *sess, const char *app, const char *stream,
                                  const char *streamid);
void zcs_srt_session_teardown_publish(zcs_srt_session *sess);
void zcs_srt_session_finish_publish(zcs_srt_session *sess);
void zcs_srt_session_drain_recv(zcs_srt_session *sess);

int zcs_srt_session_begin_play(zcs_srt_session *sess, const char *app, const char *stream,
                               const char *streamid);
void zcs_srt_session_teardown_play(zcs_srt_session *sess);
void zcs_srt_session_finish_play(zcs_srt_session *sess);
void zcs_srt_session_pump_send(zcs_srt_session *sess);

#endif /* ZCS_SRC_PROTOCOL_SRT_SERVER_INTERNAL_H */
