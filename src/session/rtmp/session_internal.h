/**
 * @file session_internal.h
 * @brief RTMP 会话私有结构（session_* 共享）。
 */
#ifndef ZMS_SRC_PROTOCOL_RTMP_SESSION_INTERNAL_H
#define ZMS_SRC_PROTOCOL_RTMP_SESSION_INTERNAL_H

#include "zms/session/rtmp/session.h"
#include "zms/protocol/rtmp/protocol.h"
#include "zms/egress/pipeline/egress_pipeline.h"
#include "zms/media/container/demux_pipeline.h"
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/core/auth.h"
#include "zms/core/gop/queue.h"
#include "zms/core/stream/buffer.h"
#include "zms/media/media_limits.h"
#include "zms/core/stream/hub.h"
#include "zms/egress/source/egress_source.h"
#include "zms/egress/pacing/egress_clock.h"
#include "zms/media/clock.h"
#include "zn/net/tcp_server.h"
#include "zb/os/sync.h"
#include <stdint.h>

/* 播放侧 VOD lane：此处不完整（LoD）；vod_rtmp_play_session.c 含完整头。 */
typedef struct zms_vod_play_lane zms_vod_play_lane;

#define ZMS_RTMP_ASYNC_START 1

typedef enum zms_rtmp_session_state {
    ZMS_RTMP_SESSION_STATE_IDLE,
    ZMS_RTMP_SESSION_STATE_PUBLISHING,
    ZMS_RTMP_SESSION_STATE_PLAYING,
} zms_rtmp_session_state;

struct zms_rtmp_session {
    unsigned session_no;
    zn_tcp_session *tcp;
    zms_rtmp_protocol *proto;
    int stream_id;
    int receive_audio;
    int receive_video;
    zms_rtmp_session_state state;
    char vhost[ZMS_VHOST_MAX];
    char app[ZMS_APP_MAX];
    char stream[ZMS_STREAM_MAX];
    zms_media_source *source;
    /** 会话生命周期（延迟销毁用 token 判活） */
    int destroy_scheduled;
    unsigned destroy_token;
    /** ── 推流侧（publish）──────────────────────────────────────────── */
    zms_live_ingest *ingress;
    void *zm_ingest; /**< zms_rtmp_zm_ingest（固定） */
    zms_demux_pipeline *publish_pipeline;

    /** ── 播放侧（play / vod）：一个独立的子结构，避免与推流状态混在一起 ── */
    struct {
        zms_egress_source src;
        zms_gop_reader *gop_reader;
        zms_egress_pipeline *egress_pipe;
        zms_vod_buffer_reader *vod_reader;
        zms_vod_play_lane *vod_lane;
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
        zms_egress_clock clk;
        zms_mux_av_timeline timeline;
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
    char params[ZMS_AUTH_PARAMS_MAX];
};

typedef struct zms_rtmp_session zms_rtmp_session;

void zms_rtmp_session_play_teardown(zms_rtmp_session *s);
void zms_rtmp_session_teardown(zms_rtmp_session *s);
void zms_rtmp_session_send(zms_rtmp_session *s, const void *data, size_t len);
void zms_rtmp_session_flush_tcp(zms_rtmp_session *s);

void zms_rtmp_session_resolve_target(zms_rtmp_session *s, const char *app_in,
                                     const char *stream_in);
struct zms_media_source;
struct zms_media_source *zms_rtmp_session_find_play_source(const char *vhost,
                                                          const char *rtmp_app,
                                                           const char *play_name, char *out_app,
                                                           size_t app_cap, char *out_stream,
                                                           size_t stream_cap);
void zms_rtmp_session_play_readers_detach(zms_rtmp_session *s);
void zms_rtmp_session_play_readers_attach(zms_rtmp_session *s);

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

void zms_rtmp_session_play_bootstrap(zms_rtmp_session *s);
void zms_rtmp_session_play_kick(zms_rtmp_session *s);
void zms_rtmp_session_schedule_play_kick(zms_rtmp_session *s);
void zms_rtmp_session_play_flush(zms_rtmp_session *s);
void zms_rtmp_session_play_flush_nolock(zms_rtmp_session *s);
void zms_rtmp_session_egress_create(zms_rtmp_session *s);
void zms_rtmp_session_egress_close(zms_rtmp_session *s);
zn_err_t zms_rtmp_session_play_vod_lane_attach(zms_rtmp_session *s, zms_media_source *src,
                                                uint64_t seek_ms);

/* --- 直播核心（rtmp_live_play_session.c）与
 *     VOD 播放路径（rtmp_vod_play_session.c）共享的会话 I/O 辅助。 --- */
int zms_rtmp_session_alive(zms_rtmp_session *s);
void zms_rtmp_session_play_fail(zms_rtmp_session *s);
void zms_rtmp_session_play_kick_finish(zms_rtmp_session *s);
void zms_rtmp_session_send_data_onstatus(zms_rtmp_session *s, const char *code, const char *desc);

/* --- VOD 播放控制辅助（rtmp_vod_play_session.c），由
 *     RTMP NetStream 命令 dispatch 驱动。 --- */
int zms_rtmp_session_vod_attach(zms_rtmp_session *s, uint64_t seek_ms);
int zms_rtmp_session_vod_replay_seek(zms_rtmp_session *s, uint64_t start_ms);
void zms_rtmp_session_play_kick_vod_prime(zms_rtmp_session *s, uint64_t start_ms);
int zms_rtmp_session_vod_lane_try_async(zms_rtmp_session *s, uint64_t start_ms);

void zms_rtmp_session_lock(zms_rtmp_session *s);
void zms_rtmp_session_unlock(zms_rtmp_session *s);
void zms_rtmp_session_publish_teardown(zms_rtmp_session *s);
void zms_rtmp_session_publisher_kick(void *ctx, int force);

#endif /* ZMS_SRC_PROTOCOL_RTMP_SESSION_INTERNAL_H */
