/**
 * @file session_internal.h
 * @brief RTMP 会话私有结构（session_* 共享）。
 */
#ifndef ZCS_SRC_PROTOCOL_RTMP_SESSION_INTERNAL_H
#define ZCS_SRC_PROTOCOL_RTMP_SESSION_INTERNAL_H

#include "zcs/session/rtmp/session.h"
#include "zcs/protocol/rtmp/protocol.h"
#include "zcs/egress/pipeline/egress_pipeline.h"
#include "zcs/media/container/demux_pipeline.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/core/auth.h"
#include "zcs/core/gop/queue.h"
#include "zcs/core/stream/buffer.h"
#include "zcs/media/media_limits.h"
#include "zcs/core/stream/hub.h"
#include "zcs/egress/source/egress_source.h"
#include "zcs/egress/pacing/egress_clock.h"
#include "zcs/media/clock.h"
#include "zn/net/tcp_server.h"
#include "zb/os/sync.h"
#include <stdint.h>

/* 播放侧 VOD lane：此处不完整（LoD）；vod_rtmp_play_session.c 含完整头。 */
typedef struct zcs_vod_play_lane zcs_vod_play_lane;

#define ZCS_RTMP_ASYNC_START 1

typedef enum zcs_rtmp_session_state {
    ZCS_RTMP_SESSION_STATE_IDLE,
    ZCS_RTMP_SESSION_STATE_PUBLISHING,
    ZCS_RTMP_SESSION_STATE_PLAYING,
} zcs_rtmp_session_state;

struct zcs_rtmp_session {
    unsigned session_no;
    zn_tcp_session *tcp;
    zcs_rtmp_protocol *proto;
    int stream_id;
    int receive_audio;
    int receive_video;
    zcs_rtmp_session_state state;
    char vhost[ZCS_VHOST_MAX];
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    zcs_media_source *source;
    /** 会话生命周期（延迟销毁用 token 判活） */
    int destroy_scheduled;
    unsigned destroy_token;
    /** ── 推流侧（publish）──────────────────────────────────────────── */
    zcs_live_ingest *ingress;
    void *zm_ingest; /**< zcs_rtmp_zm_ingest（固定） */
    zcs_demux_pipeline *publish_pipeline;

    /** ── 播放侧（play / vod）：一个独立的子结构，避免与推流状态混在一起 ── */
    struct {
        zcs_egress_source src;
        zcs_gop_reader *gop_reader;
        zcs_egress_pipeline *egress_pipe;
        zcs_vod_buffer_reader *vod_reader;
        zcs_vod_play_lane *vod_lane;
        int boot_sent;
        int video_armed;
        int live_catchup;
        uint64_t lag_resync_ms;
        int vod_catchup;
        int logged_audio;
        int logged_video;
        int reader_attached;
        /** 直播播放开始 zn_monotonic_ms()；供 on_play_stop 上报 duration_ms */
        uint64_t start_ms;
        int boot_pending;
        zcs_egress_clock clk;
        zcs_mux_av_timeline timeline;
        uint64_t vod_start_ms;
        uint32_t vod_last_seek_ms;
        uint32_t vod_pause_ms;
        int vod_pause_pos_valid;
        uint8_t *tag_buf;
        size_t tag_cap;
        uint8_t *es_buf;
        size_t es_cap;
        zb_mutex *mtx;
    } pl;
    /** 客户端 URL 查询串（鉴权 token 等），来自 connect app / publish stream 的 '?' 之后。 */
    char params[ZCS_AUTH_PARAMS_MAX];
};

typedef struct zcs_rtmp_session zcs_rtmp_session;

void zcs_rtmp_session_play_teardown(zcs_rtmp_session *s);
void zcs_rtmp_session_teardown(zcs_rtmp_session *s);
void zcs_rtmp_session_send(zcs_rtmp_session *s, const void *data, size_t len);
void zcs_rtmp_session_flush_tcp(zcs_rtmp_session *s);

void zcs_rtmp_session_resolve_target(zcs_rtmp_session *s, const char *app_in,
                                     const char *stream_in);
struct zcs_media_source;
struct zcs_media_source *zcs_rtmp_session_find_play_source(const char *vhost,
                                                          const char *rtmp_app,
                                                           const char *play_name, char *out_app,
                                                           size_t app_cap, char *out_stream,
                                                           size_t stream_cap);
void zcs_rtmp_session_play_readers_detach(zcs_rtmp_session *s);
void zcs_rtmp_session_play_readers_attach(zcs_rtmp_session *s);

int rtmp_srv_send(void *param, const void *header, size_t header_len, const void *payload,
                  size_t payload_len);
int rtmp_srv_onpublish(void *param, double transaction, const char *app_name,
                       const char *stream_name, const char *type);
int rtmp_srv_onplay(void *param, const char *app_name, const char *stream_name, double start,
                    double duration, uint8_t reset);
int rtmp_srv_onpause(void *param, int pause, uint32_t ms);
int rtmp_srv_onseek(void *param, uint32_t ms);
int rtmp_srv_onplayctrl(void *param, double speed);
int rtmp_srv_ongetduration(void *param, const char *app_name, const char *stream_name,
                           double *duration);
int rtmp_srv_onvideo(void *param, const void *data, size_t bytes, uint32_t timestamp);
int rtmp_srv_onaudio(void *param, const void *data, size_t bytes, uint32_t timestamp);
int rtmp_srv_onscript(void *param, const void *data, size_t bytes, uint32_t timestamp);

void zcs_rtmp_session_play_bootstrap(zcs_rtmp_session *s);
void zcs_rtmp_session_play_kick(zcs_rtmp_session *s);
void zcs_rtmp_session_schedule_play_kick(zcs_rtmp_session *s);
void zcs_rtmp_session_play_flush(zcs_rtmp_session *s);
void zcs_rtmp_session_play_flush_nolock(zcs_rtmp_session *s);
void zcs_rtmp_session_egress_create(zcs_rtmp_session *s);
void zcs_rtmp_session_egress_close(zcs_rtmp_session *s);
zn_err_t zcs_rtmp_session_play_vod_lane_attach(zcs_rtmp_session *s, zcs_media_source *src,
                                                uint64_t seek_ms);

/* --- 直播核心（rtmp_live_play_session.c）与
 *     VOD 播放路径（rtmp_vod_play_session.c）共享的会话 I/O 辅助。 --- */
int zcs_rtmp_session_alive(zcs_rtmp_session *s);
void zcs_rtmp_session_play_fail(zcs_rtmp_session *s);
void zcs_rtmp_session_play_kick_finish(zcs_rtmp_session *s);
void zcs_rtmp_session_send_data_onstatus(zcs_rtmp_session *s, const char *code, const char *desc);

/* --- VOD 播放控制辅助（rtmp_vod_play_session.c），由
 *     RTMP NetStream 命令 dispatch 驱动。 --- */
int zcs_rtmp_session_vod_attach(zcs_rtmp_session *s, uint64_t seek_ms);
int zcs_rtmp_session_vod_replay_seek(zcs_rtmp_session *s, uint64_t start_ms);
void zcs_rtmp_session_play_kick_vod_prime(zcs_rtmp_session *s, uint64_t start_ms);
int zcs_rtmp_session_vod_lane_try_async(zcs_rtmp_session *s, uint64_t start_ms);

void zcs_rtmp_session_lock(zcs_rtmp_session *s);
void zcs_rtmp_session_unlock(zcs_rtmp_session *s);
void zcs_rtmp_session_publish_teardown(zcs_rtmp_session *s);
void zcs_rtmp_session_publisher_kick(void *ctx, int force);

#endif /* ZCS_SRC_PROTOCOL_RTMP_SESSION_INTERNAL_H */
