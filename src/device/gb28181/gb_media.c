/**
 * @file gb_media.c
 * @brief GB28181 媒体发送：gop_queue/VOD → PS → RTP → UDP/TCP，按播放时钟发送。
 *
 * 数据通路：从源 gop_queue 读帧 → 组 Annex-B AU（IDR 前补 SPS/PPS）→
 * PS muxer 打成 PS 流 → RTP-PS 分片成 RTP 包 → UDP 直发或 TCP 加 2 字节
 * 长度前缀后发。
 *
 * 线程模型（关键）：没有专用发送线程。连接建立、发送节拍、TCP 散写、收尾
 * 全部跑在会话所属的 poller 线程上，以固定 10ms 的 tick 定时器推进；文件/回放
 * 这类阻塞 IO 才丢给线程池。因此会话字段除 refs（原子）外无锁。所有“停会话”
 * 都收敛到 poller 线程（gbm_fail → gbm_stop_inline），避免与在途回调竞争。
 */
#include "device/gb28181/gb28181_internal.h"

#include "bridge/zm_ps_muxer.h"
#include "bridge/zm_rtp_ps.h"
#include "zms/media/codec/param_sets.h"
#include "zms/media/codec/aac/aac_config.h"
#include "zms/media/codec/aac/aac_over_rtmp.h"
#include "zms/media/codec/h264/h264_es.h"
#include "zms/media/codec/h265/h265_es.h"
#include "zn/platform.h"
#include "zb/os/atomic.h"
#include "zb/os/thread.h"
#include "zn/poller/poller.h"
#include "zn/util/timer.h"
#include "zms/util/thread_pool.h"
#include "zn/util/log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET gbm_fd_t;
#define GBM_INVALID INVALID_SOCKET
#define gbm_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int gbm_fd_t;
#define GBM_INVALID (-1)
#define gbm_close close
#endif

#define GB_MEDIA_PS_MUX "ps"
#define GB_MEDIA_SOURCE_WAIT_MS 30000u

static void session_free(gb_media_session *s);

/* 发送/传输层（定义见下方 poller 驱动块；发送回调用在其前） */
static zn_poller *gbm_poller(gb_media_session *s);
static int gbm_tx_append(gb_media_session *s, const uint8_t *hdr, size_t hlen,
                         const uint8_t *data, size_t len);
static void gbm_flush_tx(gb_media_session *s);
static void gbm_on_event(int fd, unsigned events, void *user);
static uint64_t gbm_start_try(void *user);

/* 槽位/复用器（定义在节拍回调之后） */
static int media_setup_muxer(gb_media_session *s);
static int media_handle_slot(gb_media_session *s, zms_gop_slot *slot);

/* ── 套接字 ─────────────────────────────────────────────────────────────── */

static int set_nonblock(gbm_fd_t fd, int on)
{
#if defined(_WIN32)
    u_long v = on ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &v);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

/* ── 发送回调 ───────────────────────────────────────────────────────────── */

static int media_send_rtp(gb_media_session *s, const uint8_t *rtp, size_t len)
{
    if (!s || s->media_fd == -1) {
        return -1;
    }
    /* 两种传输：UDP 直接 sendto（非阻塞，缓冲满丢包不重试）；TCP 走内部 TX
     * 缓冲并加 2 字节长度前缀（GB28181 TCP 媒体承载格式），由 gbm_flush_tx
     * 分预算冲刷，避免慢平台把 poller 阻塞在 send 上。 */
    if (s->transport == GB_MEDIA_UDP) {
        struct sockaddr_in addr;
        int w;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s->remote_port);
        addr.sin_addr.s_addr = inet_addr(s->remote_ip);
        if (addr.sin_addr.s_addr == INADDR_NONE) {
            return -1;
        }
#if defined(_WIN32)
        w = sendto((gbm_fd_t)s->media_fd, (const char *)rtp, (int)len, 0,
                   (struct sockaddr *)&addr, sizeof(addr));
#else
        w = (int)sendto((gbm_fd_t)s->media_fd, rtp, len, 0, (struct sockaddr *)&addr, sizeof(addr));
#endif
        if (w < 0) {
            return -1; /* 非阻塞：缓冲区满丢这一包，下一 tick 继续 */
        }
    } else {
        uint8_t hdr[2];

        hdr[0] = (uint8_t)((len >> 8) & 0xFF);
        hdr[1] = (uint8_t)(len & 0xFF);
        if (gbm_tx_append(s, hdr, sizeof(hdr), rtp, len) != 0) {
            return -1;
        }
        gbm_flush_tx(s);
    }
    s->packets_sent++;
    s->bytes_sent += len;
    return 0;
}

static int rtp_write_cb(void *user, const uint8_t *rtp, size_t len)
{
    return media_send_rtp((gb_media_session *)user, rtp, len);
}

static int ps_write_cb(void *user, int stream_id, const uint8_t *ps, size_t len)
{
    gb_media_session *s = (gb_media_session *)user;

    (void)stream_id;
    if (!s->rtp_ps) {
        return -1;
    }
    return zms_zm_rtp_ps_input((zms_zm_rtp_ps *)s->rtp_ps, ps, len, s->cur_rtp_ts);
}

/* ── 参数集 / AU 准备 ───────────────────────────────────────────────────── */

static int reserve_au(gb_media_session *s, size_t need)
{
    /* 按需扩容 AU 缓冲（拼 SPS/PPS + 帧用）。只增不减，避免频繁 realloc；
     * 大小随最大 AU 稳定下来后基本不再触发。 */
    if (s->au_cap >= need) {
        return 0;
    }
    {
        uint8_t *p = (uint8_t *)realloc(s->au_buf, need);
        if (!p) {
            return -1;
        }
        s->au_buf = p;
        s->au_cap = need;
    }
    return 0;
}

static void params_store(zms_codec_param_sets *ps, const uint8_t *vps, size_t vps_len,
                         const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                         size_t pps_len)
{
    /* 整体替换参数集缓存：先清旧值再按非空项深拷贝。参数集很小且不常变，
     * 每次全量重建比增量维护更简单不易错。 */
    zms_codec_param_sets_clear(ps);
    if (vps && vps_len) {
        ps->vps = (uint8_t *)malloc(vps_len);
        if (ps->vps) {
            memcpy(ps->vps, vps, vps_len);
            ps->vps_len = vps_len;
        }
    }
    if (sps && sps_len) {
        ps->sps = (uint8_t *)malloc(sps_len);
        if (ps->sps) {
            memcpy(ps->sps, sps, sps_len);
            ps->sps_len = sps_len;
        }
    }
    if (pps && pps_len) {
        ps->pps = (uint8_t *)malloc(pps_len);
        if (ps->pps) {
            memcpy(ps->pps, pps, pps_len);
            ps->pps_len = pps_len;
        }
    }
}

static void media_refresh_params(gb_media_session *s, const zms_gop_slot *slot)
{
    const uint8_t *sps = NULL;
    const uint8_t *pps = NULL;
    const uint8_t *vps = NULL;
    size_t sps_len = 0;
    size_t pps_len = 0;
    size_t vps_len = 0;
    zms_codec_param_sets *ps = (zms_codec_param_sets *)s->params;

    if (!ps || !slot || !slot->data) {
        return;
    }
    /* 从码流内联的参数集刷新缓存：源可能有周期性的 SPS/PPS 更新，只有提取
     * 成功才覆盖（提取失败保留旧值）。H265 额外带 VPS。 */
    if (slot->codec == ZMS_CODEC_H264) {
        if (zms_h264_annexb_extract_sps_pps(slot->data, slot->len, &sps, &sps_len, &pps,
                                            &pps_len)) {
            params_store(ps, NULL, 0, sps, sps_len, pps, pps_len);
        }
    } else if (slot->codec == ZMS_CODEC_H265) {
        if (zms_h265_annexb_extract_vps_sps_pps(slot->data, slot->len, &vps, &vps_len, &sps,
                                                &sps_len, &pps, &pps_len)) {
            params_store(ps, vps, vps_len, sps, sps_len, pps, pps_len);
        }
    }
}

static int slot_video_is_idr(const zms_gop_slot *slot)
{
    if (!slot || !slot->data || slot->len < 5 || slot->config_frame) {
        return 0;
    }
    if (slot->codec == ZMS_CODEC_H264) {
        return slot->keyframe || zms_h264_annexb_is_idr(slot->data, slot->len);
    }
    if (slot->codec == ZMS_CODEC_H265) {
        return slot->keyframe || zms_h265_annexb_is_idr(slot->data, slot->len);
    }
    return 0;
}

static const uint8_t *media_prepare_video(gb_media_session *s, const zms_gop_slot *slot,
                                          size_t *out_len)
{
    zms_codec_param_sets *ps = (zms_codec_param_sets *)s->params;
    size_t need;

    *out_len = slot->len;
    /* 只有 IDR 需要前置参数集：非关键帧直接返回原数据。IDR 上把当前 SPS/PPS
     * （必要时先从本帧内联提取）拼到帧头，因为 GB28181 的 PS 输出里参数集应
     * 随关键帧出现，下游才能随时从 GOP 头解起。拼不下/失败就退回原帧。 */
    if (!slot_video_is_idr(slot) || !ps) {
        return slot->data;
    }
    media_refresh_params(s, slot);

    if (slot->codec == ZMS_CODEC_H264) {
        if (!ps->sps || !ps->pps || !ps->sps_len || !ps->pps_len) {
            return slot->data;
        }
        need = slot->len + ps->sps_len + ps->pps_len + 64;
        if (reserve_au(s, need) != 0) {
            return slot->data;
        }
        {
            size_t n = 0;
            zn_err_t err = zms_h264_annexb_prepend_sps_pps(
                ps->sps, ps->sps_len, ps->pps, ps->pps_len, slot->data, slot->len, s->au_buf,
                s->au_cap, &n);
            if (err == ZN_OK && n > 0) {
                *out_len = n;
                return s->au_buf;
            }
        }
        return slot->data;
    }
    if (slot->codec == ZMS_CODEC_H265) {
        int have = ps->sps && ps->sps_len && ps->pps && ps->pps_len;
        need = slot->len + ps->vps_len + ps->sps_len + ps->pps_len + 64;
        if (reserve_au(s, need) != 0) {
            return slot->data;
        }
        {
            size_t n = 0;
            zn_err_t err = zms_h265_annexb_build_rtp_au(
                ps->vps, ps->vps_len, ps->sps, ps->sps_len, ps->pps, ps->pps_len, slot->data,
                slot->len, have, s->au_buf, s->au_cap, &n);
            if (err == ZN_OK && n > 0) {
                *out_len = n;
                return s->au_buf;
            }
        }
    }
    return slot->data;
}

/* ── 会话 ───────────────────────────────────────────────────────────────── */

static void session_link(gb_media_session *s)
{
    struct gb28181_ctx *c = gb_ctx();

    /* 会话挂到全局链表（持 c->mtx）：gb_media_find_call/stop_all 跨线程按
     * call_id 遍历这张表，所有增删都必须串行。 */
    if (c->mtx) {
        zb_mutex_lock(c->mtx);
    }
    s->next = c->sessions;
    c->sessions = s;
    if (c->mtx) {
        zb_mutex_unlock(c->mtx);
    }
}

static void session_unlink(gb_media_session *s)
{
    struct gb28181_ctx *c = gb_ctx();
    gb_media_session **pp;

    if (c->mtx) {
        zb_mutex_lock(c->mtx);
    }
    for (pp = &c->sessions; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    if (c->mtx) {
        zb_mutex_unlock(c->mtx);
    }
}

gb_media_session *gb_media_find_call(const char *call_id)
{
    gb_media_session *s;
    struct gb28181_ctx *c = gb_ctx();

    if (!call_id) {
        return NULL;
    }
    /* 查表并原子加引用后再解锁返回：调用方拿到的是“已保活”的会话，即使
     * 它在 find 返回后立刻被别处 stop，也不会在调用方使用前被释放。 */
    if (c->mtx) {
        zb_mutex_lock(c->mtx);
    }
    for (s = c->sessions; s; s = s->next) {
        if (strcmp(s->call_id, call_id) == 0) {
            break;
        }
    }
    if (s) {
        (void)zb_atomic_add32(&s->refs, 1);
    }
    if (c->mtx) {
        zb_mutex_unlock(c->mtx);
    }
    return s;
}

void gb_media_session_unref(gb_media_session *s)
{
    if (!s) {
        return;
    }
    if (zb_atomic_add32(&s->refs, -1) == 0) {
        session_free(s);
    }
}

gb_media_session *gb_media_open(const char *call_id, const char *channel_id,
                                const char *platform_id, uint32_t ssrc, uint8_t pt,
                                gb_media_transport transport, const char *remote_ip,
                                uint16_t remote_port, int *out_local_port)
{
    gb_media_session *s;
    gbm_fd_t fd;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    int type = (transport == GB_MEDIA_UDP) ? SOCK_DGRAM : SOCK_STREAM;

    if (!call_id || !channel_id) {
        return NULL;
    }
    fd = socket(AF_INET, type, 0);
    if (fd == GBM_INVALID) {
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(fd, (struct sockaddr *)&addr, &alen) != 0) {
        gbm_close(fd);
        return NULL;
    }
    if (transport == GB_MEDIA_TCP_PASSIVE && listen(fd, 1) != 0) {
        /* 被动模式：本端 listen，等平台连进来再 accept（见 gbm_on_event）。 */
        gbm_close(fd);
        return NULL;
    }

    s = (gb_media_session *)calloc(1, sizeof(*s));
    if (!s) {
        gbm_close(fd);
        return NULL;
    }
    snprintf(s->call_id, sizeof(s->call_id), "%s", call_id);
    snprintf(s->channel_id, sizeof(s->channel_id), "%s", channel_id);
    snprintf(s->platform_id, sizeof(s->platform_id), "%s", platform_id ? platform_id : "");
    s->ssrc = ssrc;
    s->payload_type = pt;
    s->transport = transport;
    snprintf(s->remote_ip, sizeof(s->remote_ip), "%s", remote_ip ? remote_ip : "");
    s->remote_port = remote_port;
    s->media_fd = (int)fd;
    s->local_port = ntohs(addr.sin_port);
    if (out_local_port) {
        *out_local_port = s->local_port;
    }
    s->refs = 1; /* 会话自持的一份（最后一个引用者销毁会话） */
    s->params = calloc(1, sizeof(zms_codec_param_sets));
    if (s->params) {
        zms_codec_param_sets_init((zms_codec_param_sets *)s->params);
    }
    session_link(s);
    zn_info("gb28181: media open call=%s transport=%d local_port=%u peer=%s:%u", call_id,
             (int)transport, s->local_port, s->remote_ip, s->remote_port);
    return s;
}

int gb_media_start(gb_media_session *s, const gb_channel *ch)
{
    zn_poller *p;

    if (!s || !ch) {
        return -1;
    }
    if (s->started) {
        return 0;
    }
    snprintf(s->channel_id, sizeof(s->channel_id), "%s", ch->id);
    s->poller = gb_ctx() ? gb_ctx()->poller : NULL;
    /* 记录源上线等待截止时刻：gbm_start_try 在此时刻前每 200ms 重试，过点仍
     * 无源则放弃（默认 30s，见 GB_MEDIA_SOURCE_WAIT_MS）。 */
    s->source_deadline_ms = zn_monotonic_ms() + GB_MEDIA_SOURCE_WAIT_MS;
    if (s->media_fd != -1) {
        set_nonblock((gbm_fd_t)s->media_fd, 1);
    }
    p = gbm_poller(s);
    if (!p) {
        zn_warn("gb28181: no poller for media session channel=%s", s->channel_id);
        return -1;
    }
    s->started = 1;
    s->start_timer = zn_poller_do_delay(p, 0, gbm_start_try, s);
    if (!s->start_timer) {
        s->started = 0;
        return -1;
    }
    return 0;
}

/* ── poller 驱动传输：连接、节拍发送、TCP 散写（无专用线程） ───────────── */

#define GBM_TICK_MS 10u
#define GBM_SLOTS_PER_TICK 16
#define GBM_TX_BUDGET_PER_TICK 65536u
#define GBM_TX_CAP_MAX (512u * 1024u)
#define GBM_SOURCE_RETRY_MS 200u

static uint64_t gbm_tick_cb(void *user);
static uint64_t gbm_start_try(void *user);
static void gbm_begin_stream(gb_media_session *s);
static void gbm_connected(gb_media_session *s);
static void gbm_fail(gb_media_session *s, const char *why);
static void gbm_playback_submit(gb_media_session *s);

/* 回放：一次读一帧交给 IO 池（文件 IO 不在 poller 上），读完回到 poller 发送。 */
typedef struct gbm_pb_job {
    gb_media_session *s;
    zms_gop_slot slot;
    int r;
} gbm_pb_job;

static void gbm_pb_work(void *user)
{
    gbm_pb_job *j = (gbm_pb_job *)user;

    /* 在线程池工作线程上执行：从回放源读下一帧（可能读文件，阻塞），结果只
     * 写进 job，不触碰会话的可变发送状态。r: <0 结束/错误，==1 读到一帧。 */
    j->r = gb_playback_next(j->s->playback, &j->slot, 0);
}

static void gbm_pb_on_io(void *user)
{
    gbm_pb_job *j = (gbm_pb_job *)user;
    gb_media_session *s = j->s;

    /* 回到 poller：job 在线程池里读好的帧在这里真正发送（media_handle_slot 只
     * 在 poller 上跑）。playback_reading 复位允许下一拍再提交。清掉 reading 后
     * 才发，避免与下一拍提交竞争。 */
    s->playback_reading = 0;
    if (!s->stop) {
        if (j->r < 0) {
            gbm_fail(s, "playback end");
        } else if (j->r == 1) {
            /* 回放起播也要求从视频 IDR 开始，音频一起等，避免开播即花屏。 */
            if (!s->video_started) {
                if (j->slot.track == ZMS_TRACK_VIDEO && slot_video_is_idr(&j->slot)) {
                    s->video_started = 1;
                    s->have_pace_base = 0;
                    s->last_rtp_ts = 0;
                }
            }
            if (s->video_started) {
                (void)media_handle_slot(s, &j->slot);
            }
        }
    }
    gb_media_session_unref(s);
    free(j);
}

static void gbm_playback_submit(gb_media_session *s)
{
    zn_poller *p = gbm_poller(s);
    gbm_pb_job *j;

    if (!p || !s->playback) {
        return;
    }
    j = (gbm_pb_job *)calloc(1, sizeof(*j));
    if (!j) {
        return;
    }
    j->s = s;
    s->playback_reading = 1;
    /* 提交前给会话加一份引用：工作线程与 on_io 期间会话可能被 stop_all/外部
     * 并发释放，refs 保证任务执行时对象仍有效。提交失败要立刻回退该引用。 */
    (void)zb_atomic_add32(&s->refs, 1);
    if (zms_vod_thread_pool_run(p, gbm_pb_work, gbm_pb_on_io, j) != ZN_OK) {
        s->playback_reading = 0;
        gb_media_session_unref(s);
        free(j);
    }
}

/* 回放：打开 VOD 源（文件 IO 也走 IO 池）。 */
static void gbm_pb_open_work(void *user)
{
    gb_media_session *s = (gb_media_session *)user;

    s->playback = gb_playback_open(s->channel_id, s->play_start, s->play_end, gbm_poller(s));
}

static void gbm_pb_open_on_io(void *user)
{
    gb_media_session *s = (gb_media_session *)user;

    if (!s->stop) {
        if (!s->playback) {
            gbm_fail(s, "playback open failed");
        } else {
            s->source = gb_playback_source(s->playback);
            if (!s->source) {
                gbm_fail(s, "playback source missing");
            } else {
                s->video_codec = s->source->video.codec;
                s->audio_codec = s->source->audio.codec;
                gbm_begin_stream(s);
            }
        }
    }
    gb_media_session_unref(s);
}

static void gbm_playback_open_submit(gb_media_session *s)
{
    zn_poller *p = gbm_poller(s);

    if (!p) {
        gbm_fail(s, "no poller");
        return;
    }
    /* 打开回放源也要读文件/建索引，丢线程池；同样先加引用保证 on_io 执行前
     * 会话不被释放。提交失败回退引用并 fail。 */
    (void)zb_atomic_add32(&s->refs, 1);
    if (zms_vod_thread_pool_run(p, gbm_pb_open_work, gbm_pb_open_on_io, s) != ZN_OK) {
        gb_media_session_unref(s);
        gbm_fail(s, "playback open submit failed");
    }
}

/* 测试注入：无 GB 上下文时也能拿到 poller。 */
static zn_poller *g_gbm_poller_override;

void gb_media_set_poller_for_test(zn_poller *p)
{
    g_gbm_poller_override = p;
}

static zn_poller *gbm_poller(gb_media_session *s)
{
    zn_poller *p;

    if (g_gbm_poller_override) {
        return g_gbm_poller_override;
    }
    p = s ? s->poller : NULL;
    if (!p) {
        struct gb28181_ctx *c = gb_ctx();
        p = c ? c->poller : NULL;
    }
    return p;
}

static int gbm_send_once(gbm_fd_t fd, const uint8_t *data, size_t len)
{
#if defined(_WIN32)
    return send(fd, (const char *)data, (int)len, 0);
#else
    return (int)send(fd, data, len, 0);
#endif
}

/* 把一条已封装的报文追加到 TX 缓冲（TCP 用；UDP 直接发）。 */
static int gbm_tx_append(gb_media_session *s, const uint8_t *hdr, size_t hlen,
                         const uint8_t *data, size_t len)
{
    size_t need = hlen + len;

    if (s->tx_off > 0) {
        memmove(s->tx_buf, s->tx_buf + s->tx_off, s->tx_len - s->tx_off);
        s->tx_len -= s->tx_off;
        s->tx_off = 0;
    }
    if (s->tx_len + need > GBM_TX_CAP_MAX) {
        return -1; /* 平台读得太慢：丢弃本条，避免缓冲无限增长 */
    }
    if (s->tx_cap < s->tx_len + need) {
        size_t cap = s->tx_cap ? s->tx_cap : 8192;
        uint8_t *p;

        while (cap < s->tx_len + need) {
            cap *= 2;
        }
        if (cap > GBM_TX_CAP_MAX) {
            cap = GBM_TX_CAP_MAX;
        }
        p = (uint8_t *)realloc(s->tx_buf, cap);
        if (!p) {
            return -1;
        }
        s->tx_buf = p;
        s->tx_cap = cap;
    }
    if (hlen) {
        memcpy(s->tx_buf + s->tx_len, hdr, hlen);
    }
    memcpy(s->tx_buf + s->tx_len + hlen, data, len);
    s->tx_len += need;
    return 0;
}

/* 非阻塞冲刷 TX 缓冲：单次上限 GBM_TX_BUDGET_PER_TICK，剩余留到下个 tick。 */
static void gbm_flush_tx(gb_media_session *s)
{
    size_t budget = GBM_TX_BUDGET_PER_TICK;

    while (s->media_fd != -1 && s->tx_off < s->tx_len && budget > 0) {
        size_t n = s->tx_len - s->tx_off;
        int w;

        if (n > budget) {
            n = budget;
        }
        w = gbm_send_once((gbm_fd_t)s->media_fd, s->tx_buf + s->tx_off, n);
        if (w <= 0) {
            break; /* EAGAIN 或错误：下个 tick 再试 */
        }
        s->tx_off += (size_t)w;
        budget -= (size_t)w;
    }
    if (s->tx_off >= s->tx_len) {
        s->tx_off = 0;
        s->tx_len = 0;
    }
}

/* 收尾（仅在会话所属 poller 线程执行）：摘链、停定时器、注销事件、释放自持引用。
 * 定时器回调与 fd 事件都在该线程上串行执行，因此在此释放会话不会与在途回调竞争。 */
static void gbm_stop_inline(gb_media_session *s)
{
    zn_poller *p;

    session_unlink(s);
    if (s->start_timer) {
        zn_poller_timer_cancel(s->start_timer);
        s->start_timer = NULL;
    }
    if (s->tick_timer) {
        zn_poller_timer_cancel(s->tick_timer);
        s->tick_timer = NULL;
    }
    p = gbm_poller(s);
    if (s->fd_events && p && s->media_fd != -1) {
        zn_poller_del_sync(p, (int)s->media_fd);
        s->fd_events = 0;
    }
    if (s->reader_valid) {
        zms_media_subscriber_detach(&s->reader);
        s->reader_valid = 0;
    }
    gb_media_session_unref(s); /* 释放会话自持引用 */
}

/* 跨线程收尾任务：持有一份引用，保证任务执行前会话有效。 */
static void gbm_stop_job(void *user)
{
    gb_media_session *s = (gb_media_session *)user;

    gbm_stop_inline(s);
    gb_media_session_unref(s); /* 归还任务持有的引用 */
}

/* 会话结束（可来自任意路径）：先置 stop 止血，收尾一律回到 poller 线程，
 * 避免与在途的定时器/fd 回调竞争导致 UAF。 */
static void gbm_fail(gb_media_session *s, const char *why)
{
    zn_poller *p;

    if (!s) {
        return;
    }
    if (why && !s->stop) {
        zn_warn("gb28181: media stop channel=%s: %s", s->channel_id, why);
    }
    if (s->stop) {
        return;
    }
    s->stop = 1;

    p = gbm_poller(s);
    if (p && zn_poller_is_running(p) && !zn_poller_is_current_thread(p)) {
        (void)zb_atomic_add32(&s->refs, 1); /* 任务持有 */
        if (zn_poller_async(p, gbm_stop_job, s, 0) == ZN_OK) {
            return;
        }
        (void)zb_atomic_add32(&s->refs, -1);
    }
    gbm_stop_inline(s);
}

static void gbm_update_events(gb_media_session *s, unsigned events)
{
    zn_poller *p = gbm_poller(s);

    if (!p || s->media_fd == -1) {
        return;
    }
    /* 统一维护 fd 的 poller 关注事件：0=全部注销，之前没注册则 add，事件变了
     * 则 mod。这样 accept/connect/收尾各路径都只需调一次本函数。 */
    if (events == 0) {
        if (s->fd_events) {
            zn_poller_del_sync(p, (int)s->media_fd);
            s->fd_events = 0;
        }
        return;
    }
    if (s->fd_events == 0) {
        if (zn_poller_add(p, (int)s->media_fd, events, gbm_on_event, s) == ZN_OK) {
            s->fd_events = events;
        }
    } else if (s->fd_events != events) {
        if (zn_poller_mod(p, (int)s->media_fd, events) == ZN_OK) {
            s->fd_events = events;
        }
    }
}

/* 套接字事件：TCP 连接完成 / 被动模式 accept；其余情况不读（发送方）。 */
static void gbm_on_event(int fd, unsigned events, void *user)
{
    gb_media_session *s = (gb_media_session *)user;

    if (!s || s->stop) {
        return;
    }
    if (events & (ZN_POLL_ERR | ZN_POLL_HUP)) {
        gbm_fail(s, "socket error");
        return;
    }
    if (s->accepting && (events & ZN_POLL_IN)) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        gbm_fd_t c;

        c = accept((gbm_fd_t)fd, (struct sockaddr *)&peer, &plen);
        if (c == GBM_INVALID) {
            return; /* EAGAIN */
        }
        /* 被动模式只接一个平台连接：accept 后注销监听、关掉 listening fd，
         * 用新 fd 替换 media_fd 继续承载媒体，避免继续收连接。 */
        set_nonblock(c, 1);
        gbm_update_events(s, 0);
        gbm_close((gbm_fd_t)fd);
        s->media_fd = (int)c;
        s->accepting = 0;
        {
            char ipbuf[64];

            (void)inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));
            zn_info("gb28181: TCP accepted channel=%s peer=%s:%u", s->channel_id, ipbuf,
                     (unsigned)ntohs(peer.sin_port));
        }
        gbm_connected(s);
        return;
    }
    if (s->connecting && (events & ZN_POLL_OUT)) {
        int err = 0;
        socklen_t el = sizeof(err);

        /* 非阻塞 connect 完成：用 SO_ERROR 判断成功还是失败（此时 OUT 事件
         * 既代表“连上了”也可能藏着一个延迟错误）。 */
        getsockopt((gbm_fd_t)fd, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err != 0) {
            gbm_fail(s, "TCP connect failed");
            return;
        }
        gbm_connected(s);
    }
}

/* 等源上线：通道/源还没发布时每 200ms 重试，超时放弃。 */
static uint64_t gbm_start_try(void *user)
{
    gb_media_session *s = (gb_media_session *)user;
    gb_channel ch;
    zms_media_source *src;

    /* 单次执行（返回 0 不再自续）：成功则交棒给 gbm_begin_stream 里的发送
     * 节拍定时器；失败重试则自己排下一次 200ms。回放模式则先异步打开回放源，
     * 打开完成再回到这里继续。 */
    if (!s || s->stop) {
        return 0;
    }
    s->start_timer = NULL;
    if (s->is_playback && !s->playback) {
        gbm_playback_open_submit(s);
        return 0;
    }
    /* 源已上线：每 200ms 重试直到发布，或到 source_deadline_ms 超时放弃。
     * 这一步把“通道暂时没源”和“真的挂了”区分开：期内重试、超期才 fail。 */
    if (gb_channel_get(s->channel_id, &ch) != 0) {
        gbm_fail(s, "channel missing");
        return 0;
    }
    src = gb_channel_source(&ch);
    if (!src || !src->gop_queue || !src->publishing) {
        if (zn_monotonic_ms() < s->source_deadline_ms) {
            zn_poller *p = gbm_poller(s);

            if (p) {
                s->start_timer = zn_poller_do_delay(p, GBM_SOURCE_RETRY_MS, gbm_start_try, s);
                if (s->start_timer) {
                    return 0;
                }
            }
        }
        gbm_fail(s, "source not online");
        return 0;
    }
    s->source = src;
    s->video_codec = src->video.codec;
    s->audio_codec = src->audio.codec;
    gbm_begin_stream(s);
    return 0; /* 单次：后续由发送节拍定时器接管 */
}

/* 源已就绪：建 muxer、挂读者、发起连接，连接完成后进入发送节拍。 */
static void gbm_begin_stream(gb_media_session *s)
{
    zn_poller *p;

    if (!s || s->stop) {
        return;
    }
    if (media_setup_muxer(s) != 0) {
        gbm_fail(s, "muxer setup failed");
        return;
    }
    if (!s->is_playback) {
        if (zms_media_source_subscribe_gop(s->source, &s->reader) != ZN_OK) {
            gbm_fail(s, "attach reader failed");
            return;
        }
        s->reader_valid = 1;
        /* 从最近的 IDR 起读；若读者落后超过 240 个 slot（约数秒，来源卡顿或
         * 刚起播），改为 seek_live 直接跳到直播点，避免从过旧 GOP 开始追不上。 */
        zms_gop_reader_seek_rtsp_idr(s->reader.gop);
        if (zms_gop_reader_lag(s->reader.gop) > 240) {
            zms_gop_reader_seek_live(s->reader.gop);
        }
    }

    /* 连接：UDP 无连接直接可发；TCP-ACTIVE 非阻塞 connect；PASSIVE 等 accept。 */
    if (s->transport == GB_MEDIA_UDP) {
        gbm_connected(s);
    } else if (s->transport == GB_MEDIA_TCP_ACTIVE) {
        struct sockaddr_in addr;
        int rc;

        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(s->remote_port);
        addr.sin_addr.s_addr = inet_addr(s->remote_ip);
        if (addr.sin_addr.s_addr == INADDR_NONE) {
            gbm_fail(s, "bad remote ip");
            return;
        }
        rc = connect((gbm_fd_t)s->media_fd, (struct sockaddr *)&addr, sizeof(addr));
        /* connect 返回非 0 且是 EINPROGRESS 时，注册 OUT 事件等完成（见
         * gbm_on_event）；UDP/已连上则直接进发送节拍。 */
        if (rc != 0) {
            s->connecting = 1;
            gbm_update_events(s, ZN_POLL_OUT | ZN_POLL_ERR);
        } else {
            gbm_connected(s);
        }
    } else {
        s->accepting = 1;
        gbm_update_events(s, ZN_POLL_IN | ZN_POLL_ERR);
    }
}

/* 连接就绪（UDP 直接可用 / TCP connect 或 accept 完成）：开始发送节拍。 */
static void gbm_connected(gb_media_session *s)
{
    zn_poller *p;

    if (!s || s->stop || s->media_connected) {
        return;
    }
    /* 幂等：UDP 立即算“已连”，TCP-active 在 connect 完成/accept 完成时调用。
     * 第一次进入才起发送节拍定时器（10ms 自续），后续重复调用直接返回。 */
    s->media_connected = 1;
    p = gbm_poller(s);
    if (p && !s->tick_timer) {
        s->tick_timer = zn_poller_do_delay(p, GBM_TICK_MS, gbm_tick_cb, s);
    }
    zn_info("gb28181: streaming channel=%s video=%s audio=%s ssrc=%u transport=%s",
             s->channel_id, zms_codec_name(s->video_codec), zms_codec_name(s->audio_codec),
             s->ssrc, s->transport == GB_MEDIA_UDP ? "udp" : "tcp");
}

/* 发送节拍：poller 上非阻塞推进，单 tick 处理帧数有上限。 */
static uint64_t gbm_tick_cb(void *user)
{
    gb_media_session *s = (gb_media_session *)user;
    int i;

    if (!s || s->stop) {
        return 0;
    }
    if (!s->media_connected) {
        return GBM_TICK_MS;
    }
    if (s->transport != GB_MEDIA_UDP) {
        gbm_flush_tx(s);
    }
    if (s->is_playback) {
        /* 回放：按节拍提交一次异步读（文件 IO 在 IO 池）。 */
        if (!s->playback_reading && s->playback && zn_monotonic_ms() >= s->next_due_ms) {
            gbm_playback_submit(s);
        }
        return GBM_TICK_MS;
    }
    if (!s->reader_valid) {
        return GBM_TICK_MS;
    }
    for (i = 0; i < GBM_SLOTS_PER_TICK; ++i) {
        zms_gop_slot slot;
        int r = zms_gop_reader_read_muxed(s->reader.gop, &slot, 0);

        if (r <= 0) {
            break;
        }
        if (!slot.data || slot.len == 0) {
            continue;
        }
        if (!s->video_started) {
            if (slot.track == ZMS_TRACK_VIDEO && slot_video_is_idr(&slot)) {
                s->video_started = 1;
                s->have_pace_base = 0;
                s->last_rtp_ts = 0;
            } else {
                continue; /* 起播必须从 IDR 开始，音视频一起等 */
            }
        }
        (void)media_handle_slot(s, &slot);
    }
    if (s->transport != GB_MEDIA_UDP) {
        gbm_flush_tx(s);
    }
    if (s->stop) {
        /* 返回 0 后 poller 会释放该定时器；清掉句柄，避免后续 stop 路径
         * 再对悬空指针 zn_poller_timer_cancel（ASan: heap-use-after-free）。 */
        s->tick_timer = NULL;
        return 0;
    }
    return GBM_TICK_MS; /* 固定节拍，由 poller 延迟任务自续 */
}

static int media_setup_muxer(gb_media_session *s)
{
    zms_zm_ps_muxer_opts popts;
    zms_zm_rtp_ps_opts ropts;
    zms_codec_param_sets *ps = (zms_codec_param_sets *)s->params;

    memset(&popts, 0, sizeof(popts));
    popts.write = ps_write_cb;
    popts.user = s;
    s->ps_mux = zms_zm_ps_muxer_create(&popts);
    if (!s->ps_mux) {
        return -1;
    }
    memset(&ropts, 0, sizeof(ropts));
    ropts.payload_type = s->payload_type;
    ropts.ssrc = s->ssrc;
    /* 初始 seq 用单调时钟低 16 位：随机化起点，避免重启后与上次会话的 RTP
     * 序号连续导致对端误判。mtu=1400 留出 RTP/PS 头空间（以太网 1500 内）。 */
    ropts.seq = (uint16_t)(zn_monotonic_ms() & 0xFFFF);
    ropts.mtu = 1400;
    ropts.write = rtp_write_cb;
    ropts.user = s;
    s->rtp_ps = zms_zm_rtp_ps_create(&ropts);
    if (!s->rtp_ps) {
        return -1;
    }

    if (s->video_codec == ZMS_CODEC_H264 || s->video_codec == ZMS_CODEC_H265) {
        s->video_stream = zms_zm_ps_muxer_add_video((zms_zm_ps_muxer *)s->ps_mux,
                                                      s->video_codec);
        if (s->video_stream <= 0) {
            return -1;
        }
        if (ps) {
            size_t cfg_len = 0;
            const uint8_t *cfg = s->source ? zms_media_source_video_config(s->source, &cfg_len)
                                           : NULL;
            /* 直播源优先取源的视频 config；回放没有直播源时退回回放源的 config。
             * 缓存进 ps（参数集），供 media_prepare_video 在 IDR 前 prepend。 */
            if ((!cfg || !cfg_len) && s->playback) {
                cfg = gb_playback_video_config(s->playback, &cfg_len);
            }
            if (cfg && cfg_len) {
                zms_codec_param_sets_cache_flv_cfg(ps, cfg, cfg_len);
            }
        }
    }
    if (s->audio_codec == ZMS_CODEC_AAC || s->audio_codec == ZMS_CODEC_G711A ||
        s->audio_codec == ZMS_CODEC_G711U) {
        s->audio_stream = zms_zm_ps_muxer_add_audio((zms_zm_ps_muxer *)s->ps_mux,
                                                     s->audio_codec);
    }
    if (s->audio_codec == ZMS_CODEC_AAC && s->source) {
        size_t alen = 0;
        const uint8_t *acfg = zms_media_source_audio_config(s->source, &alen);
        if ((!acfg || !alen) && s->playback) {
            acfg = gb_playback_audio_config(s->playback, &alen);
        }
        const uint8_t *asc = NULL;
        size_t ascl = 0;
        zms_aac_config *cfg = (zms_aac_config *)calloc(1, sizeof(*cfg));
        if (cfg) {
            int ok = 0;
            /* ASC 可能以 FLV 音频头（带 tag 字节）形式给出，也可能就是裸 ASC：
             * 先尝试剥 FLV 头，失败再当裸 ASC 解析。都失败用源声道/采样率的
             * 默认参数兜底，至少让音频能出。 */
            if (acfg && alen) {
                if (zms_rtmp_aac_extradata(acfg, alen, &asc, &ascl)) {
                    ok = zms_aac_config_load_asc(cfg, asc, ascl);
                } else {
                    ok = zms_aac_config_load_asc(cfg, acfg, alen);
                }
            }
            if (!ok) {
                zms_aac_config_set_defaults(cfg, s->source->audio.sample_rate, s->source->audio.channels);
            }
            s->aac_cfg = cfg;
        }
    }
    return 0;
}

static const uint8_t *media_prepare_audio(gb_media_session *s, const zms_gop_slot *slot,
                                          size_t *out_len)
{
    zms_aac_config *cfg = (zms_aac_config *)s->aac_cfg;
    uint8_t hdr[16];
    int hn;
    size_t need;

    *out_len = slot->len;
    if (slot->codec != ZMS_CODEC_AAC) {
        return slot->data;
    }
    if (!cfg) {
        return NULL; /* 无 ASC，无法生成 ADTS，丢弃音频避免脏流 */
    }
    /* PS 里的 AAC 需为 ADTS 封装：每帧前加 7 字节 ADTS 头（含采样率/声道/
     * 帧长），从 ASC 反推。头生成失败就退回裸帧（少见，交给下游容忍）。 */
    hn = zms_aac_config_adts_header(cfg, (uint16_t)slot->len, hdr, sizeof(hdr));
    if (hn <= 0) {
        return slot->data;
    }
    need = (size_t)hn + slot->len;
    if (s->aud_cap < need) {
        uint8_t *p = (uint8_t *)realloc(s->aud_buf, need);
        if (!p) {
            return NULL;
        }
        s->aud_buf = p;
        s->aud_cap = need;
    }
    memcpy(s->aud_buf, hdr, (size_t)hn);
    memcpy(s->aud_buf + hn, slot->data, slot->len);
    *out_len = need;
    return s->aud_buf;
}

static int media_handle_slot(gb_media_session *s, zms_gop_slot *slot)
{
    uint32_t dts;
    uint32_t pts;
    uint32_t rel_dts;
    uint32_t rel_pts;
    int64_t pts90;
    int64_t dts90;

    if (!slot->data || slot->len == 0) {
        return 0;
    }
    if (slot->track == ZMS_TRACK_VIDEO) {
        /* 参数集帧不单独发，IDR 前统一 prepend。 */
        if (slot->config_frame) {
            return 0;
        }
        /* 只支持 H264/H265 出 PS；其它编码（如 AV1）无法封进 GB28181 PS，
         * 直接丢弃而不是发脏流。 */
        if (slot->codec != ZMS_CODEC_H264 && slot->codec != ZMS_CODEC_H265) {
            return 0;
        }
    } else if (slot->track == ZMS_TRACK_AUDIO) {
        if (slot->config_frame) {
            return 0;
        }
        /* 没有建出音频流（源无音轨/不支持的音频编码）时静默丢弃音频帧。 */
        if (!s->audio_stream) {
            return 0;
        }
    } else {
        return 0;
    }

    dts = slot->dts_ms;
    pts = slot->pts_ms ? slot->pts_ms : dts;
    /* 以首帧 dts 为基准换算相对 90kHz 时间戳，使 RTP 时间戳从 0 起。
     * first_wall_ms 记录“首帧的墙钟”，回放用 next_due_ms 判断何时才该发下一帧。 */
    if (!s->have_pace_base) {
        s->first_dts_ms = dts;
        s->first_wall_ms = zn_monotonic_ms();
        s->have_pace_base = 1;
    }
    rel_dts = (uint32_t)(dts - s->first_dts_ms);
    rel_pts = (uint32_t)(pts - s->first_dts_ms);
    dts90 = (int64_t)rel_dts * 90;
    pts90 = (int64_t)rel_pts * 90;

    s->cur_rtp_ts = (uint32_t)dts90;
    /* 时间戳只许单调不减：B 帧/抖动可能让某些 dts 倒退，给 RTP 会乱序；钳到
     * 上一个值，保证下游时间线单调。 */
    if (s->cur_rtp_ts < s->last_rtp_ts) {
        s->cur_rtp_ts = s->last_rtp_ts;
    }
    s->last_rtp_ts = s->cur_rtp_ts;

    /* 回放按节拍发送：记录该帧最早可发时刻。 */
    s->next_due_ms = s->first_wall_ms + rel_dts;

    if (slot->track == ZMS_TRACK_VIDEO) {
        size_t au_len = 0;
        const uint8_t *au = media_prepare_video(s, slot, &au_len);
        if (!au || !s->video_stream) {
            return 0;
        }
        return zms_zm_ps_muxer_input_video((zms_zm_ps_muxer *)s->ps_mux, s->video_stream, au,
                                            au_len, slot_video_is_idr(slot), pts90, dts90);
    }
    {
        size_t alen = 0;
        const uint8_t *au = media_prepare_audio(s, slot, &alen);
        if (!au) {
            return 0;
        }
        return zms_zm_ps_muxer_input_audio((zms_zm_ps_muxer *)s->ps_mux, s->audio_stream, au,
                                            alen, pts90);
    }
}

static void session_free(gb_media_session *s)
{
    zn_poller *p;

    if (!s) {
        return;
    }
    /* 引用归零后的最终释放（由 gb_media_session_unref 在 refs==0 时调用）。
     * 此时会话已从 poller 摘除，这里只做资源析构：定时器、fd 事件、socket、
     * muxer、回放源、参数集、各缓冲。 */
    p = gbm_poller(s);
    if (s->start_timer) {
        zn_poller_timer_cancel(s->start_timer);
        s->start_timer = NULL;
    }
    if (s->tick_timer) {
        zn_poller_timer_cancel(s->tick_timer);
        s->tick_timer = NULL;
    }
    if (s->fd_events && p && s->media_fd != -1) {
        zn_poller_del_sync(p, (int)s->media_fd);
        s->fd_events = 0;
    }
    if (s->media_fd != -1) {
        gbm_close((gbm_fd_t)s->media_fd);
        s->media_fd = -1;
    }
    if (s->ps_mux) {
        zms_zm_ps_muxer_destroy((zms_zm_ps_muxer *)s->ps_mux);
        s->ps_mux = NULL;
    }
    if (s->rtp_ps) {
        zms_zm_rtp_ps_destroy((zms_zm_rtp_ps *)s->rtp_ps);
        s->rtp_ps = NULL;
    }
    if (s->playback) {
        gb_playback_close(s->playback);
        s->playback = NULL;
    }
    if (s->params) {
        zms_codec_param_sets_clear((zms_codec_param_sets *)s->params);
        free(s->params);
        s->params = NULL;
    }
    free(s->au_buf);
    free(s->aud_buf);
    free(s->aac_cfg);
    free(s->tx_buf);
    free(s);
}

int gb_media_stop_call(const char *call_id)
{
    gb_media_session *s = gb_media_find_call(call_id);

    if (!s) {
        return 0;
    }
    if (!s->stop) {
        /* 停定时器、注销事件、摘链并释放会话自持引用（可能在此释放）。 */
        gbm_fail(s, NULL);
    }
    gb_media_session_unref(s); /* 放开 find_call 取得的那份引用 */
    return 1;
}

void gb_media_stop_all(void)
{
    struct gb28181_ctx *c = gb_ctx();

    for (;;) {
        gb_media_session *s;

        if (!c) {
            break;
        }
        if (c->mtx) {
            zb_mutex_lock(c->mtx);
        }
        s = c->sessions;
        if (s) {
            c->sessions = s->next;
        }
        if (c->mtx) {
            zb_mutex_unlock(c->mtx);
        }
        if (!s) {
            break;
        }
        /* 已置 stop 的会话：其收尾任务（跨线程）或 inline 收尾已持有/释放引用，
         * 这里不能再次 unref，否则与任务释放重复。 */
        if (!s->stop) {
            gbm_fail(s, NULL);
        }
    }
}

int gb_media_session_count(void)
{
    gb_media_session *s;
    int n = 0;
    struct gb28181_ctx *c = gb_ctx();

    if (c->mtx) {
        zb_mutex_lock(c->mtx);
    }
    for (s = c->sessions; s; s = s->next) {
        ++n;
    }
    if (c->mtx) {
        zb_mutex_unlock(c->mtx);
    }
    return n;
}
