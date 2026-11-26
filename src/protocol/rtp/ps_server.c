/**
 * @file ps_server.c
 * @brief RTP-PS 服务端注册表（按 vhost/app/stream 查找）。
 */
#include "zms/protocol/rtp/ps_server.h"
#include "zms/util/dns_cache.h"
#include "protocol/rtp/ps_server_internal.h"
#include "zms/ops/api/webhook/webhook_client.h"
#include "zms/egress/recorder/egress_mp4_recorder.h"
#include "zms/core/event.h"
#include "zms/media/wire/rtp_packet.h"
#include "zms/publish/ingest/common/stream_ingest.h"
#include "zms/util/buf_pool.h"
#include "rtp-payload.h"
#include "zn/net/tcp_client.h"
#include "zn/net/tcp_server.h"
#include "zn/net/udp_server.h"
#include "zn/net/socket.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZMS_RTP_PS_RECV_BUF 65536
#define ZMS_RTP_PS_TCP_ASM_INIT 65536

typedef struct zms_rtp_ps_server_reg_entry {
    char key[256];
    zms_rtp_ps_server_slot *slot;
    struct zms_rtp_ps_server_reg_entry *next;
} zms_rtp_ps_server_reg_entry;

/*
 * 全局注册表用单链表，加/删/查都不加锁。前提是这些操作都发生在同一个
 * “信令 poller”线程（GB28181 的 openRtpServer/close 由信令驱动，串行执行）。
 * 若将来允许跨线程增删，这里必须补锁——但找 slot 的读路径在 UDP/TCP 收包
 * 回调里也很热，加锁会带来争用，故保持单线程假设并把它显式记在这里。
 */
static zms_rtp_ps_server_reg_entry *g_rtp_ps_registry;
static unsigned g_rtp_ps_session_no;

static void rtp_ps_reg_add(zms_rtp_ps_server_slot *slot)
{
    zms_rtp_ps_server_reg_entry *e;

    if (!slot || !slot->key[0]) {
        return;
    }
    e = (zms_rtp_ps_server_reg_entry *)calloc(1, sizeof(*e));
    if (!e) {
        return;
    }
    strncpy(e->key, slot->key, sizeof(e->key) - 1);
    e->slot = slot;
    e->next = g_rtp_ps_registry;
    g_rtp_ps_registry = e;
}

static void rtp_ps_reg_remove(zms_rtp_ps_server_slot *slot)
{
    zms_rtp_ps_server_reg_entry **pp = &g_rtp_ps_registry;

    if (!slot) {
        return;
    }
    while (*pp) {
        if ((*pp)->slot == slot) {
            zms_rtp_ps_server_reg_entry *dead = *pp;
            *pp = dead->next;
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* 注册表键：vhost/app/stream 归一化拼串。空 vhost 用 ZLM 约定的 "__defaultVhost__"，
 * 空 app 默认 "live"，与其它协议模块的键保持一致，便于跨协议按同一 key 查找。 */
void zms_rtp_ps_server_make_key(const char *vhost, const char *app, const char *stream, char *key,
                                size_t key_cap)
{
    const char *vh = (vhost && vhost[0]) ? vhost : "__defaultVhost__";
    if (!key || key_cap == 0) {
        return;
    }
    snprintf(key, key_cap, "%s/%s/%s", vh, app && app[0] ? app : "live",
             stream && stream[0] ? stream : "");
}

zms_rtp_ps_server_slot *zms_rtp_ps_server_find_by_key(const char *key)
{
    if (!key || !key[0]) {
        return NULL;
    }
    for (zms_rtp_ps_server_reg_entry *e = g_rtp_ps_registry; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e->slot;
        }
    }
    return NULL;
}

zms_rtp_ps_server_slot *zms_rtp_ps_server_slot_from_udp(zn_udp_server *udp)
{
    for (zms_rtp_ps_server_reg_entry *e = g_rtp_ps_registry; e; e = e->next) {
        if (e->slot && e->slot->udp == udp) {
            return e->slot;
        }
    }
    return NULL;
}

zms_media_source *zms_rtp_ps_server_source(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->source : NULL;
}

uint16_t zms_rtp_ps_server_port(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->port : 0;
}

const char *zms_rtp_ps_server_app(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->app : "";
}

const char *zms_rtp_ps_server_stream(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->stream : "";
}

int zms_rtp_ps_server_payload_type(const zms_rtp_ps_server_slot *slot)
{
    return slot ? slot->payload_type : ZMS_RTP_PS_DEFAULT_PT;
}

int zms_rtp_ps_server_foreach(zms_rtp_ps_server_visit_fn fn, void *user)
{
    int n = 0;
    if (!fn) {
        return 0;
    }
    for (zms_rtp_ps_server_reg_entry *e = g_rtp_ps_registry; e; e = e->next) {
        if (e->slot && fn(e->key, e->slot, user) != 0) {
            break;
        }
        ++n;
    }
    return n;
}

static void rtp_ps_slot_commit_publish(zms_rtp_ps_server_slot *slot)
{
    zms_media_event_publish(slot->source, ZMS_ORIGIN_RTP_PS_PUSH);
    zms_media_source_set_publisher(slot->source, slot, NULL);
    if (slot->source->enable_mp4) {
        (void)zms_mp4_recorder_start(slot->source, slot->poller);
    }
    slot->publish_started = 1;
    zn_info("[rtp-ps] publish ready app=%s stream=%s port=%u tcp_mode=%d enable_mp4=%d", slot->app,
             slot->stream, (unsigned)slot->port, slot->tcp_mode, slot->source->enable_mp4);
}

static void rtp_ps_slot_finish_publish(zms_rtp_ps_server_slot *slot)
{
    if (!slot || slot->publish_started || !slot->source) {
        return;
    }
    /* 信令协商槽位默认录制；外部直推按开槽时验签结论决定。 */
    slot->source->enable_mp4 = slot->signaling_authorized ? 1 : slot->auth_enable_mp4;
    rtp_ps_slot_commit_publish(slot);
}

/*
 * 首个视频关键帧到达后才真正“提交发布”。
 *
 * 为什么不在收到第一个 RTP 包时就发布：PS 流里关键帧之前的帧无法解码，播放端
 * 若在这些帧发布时接入会拿到不可解数据。等首个关键帧（此刻 SPS/PPS 也已通过
 * on_h264_ps 拿到）再 publish，订阅者一进来就是可解码的起点。
 * 顺带在这里决定是否录制：信令协商的槽位默认录，外部直推则按开槽时的鉴权结论。
 */
static void rtp_ps_on_ps_frame(const zms_frame *frame, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;

    if (!slot || !slot->ingress || !frame || frame->size == 0) {
        return;
    }
    if (zms_live_ingest_input_frame(slot->ingress, frame) != ZN_OK) {
        return;
    }
    if (frame->track == ZMS_TRACK_VIDEO && frame->keyframe) {
        rtp_ps_slot_finish_publish(slot);
    }
}

static void rtp_ps_on_h264_ps(const uint8_t *sps, size_t sps_len, const uint8_t *pps,
                              size_t pps_len, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;

    if (!slot || !slot->ingress) {
        return;
    }
    (void)zms_live_ingest_set_h264_sps_pps(slot->ingress, sps, sps_len, pps, pps_len);
}

/*
 * 组装一个 PS 包用的缓冲区。每次按需 resize 后**复用同一块** slot->rtp_ps_assembly，
 * 而不是每次 malloc：收包路径每包一次分配会成为高频热点。free 回调是空操作，
 * 因为缓冲区归 slot 所有、下次 alloc 会覆盖，直到 slot 销毁统一清理。
 * 代价是 decoders 不能跨 alloc 保留多个包同时引用——但 rtp_payload_decode 是
 * 逐包同步解码，拿到 packet 后立即消费，不存在并发持有多包的情况。
 */
static void *rtp_ps_payload_alloc(void *param, int bytes)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)param;

    if (!slot || bytes <= 0) {
        return NULL;
    }
    if (!zms_buf_pool_slot_resize(&slot->rtp_ps_assembly, &slot->rtp_ps_assembly_cap,
                                  (size_t)bytes)) {
        return NULL;
    }
    return slot->rtp_ps_assembly;
}

static void rtp_ps_payload_free(void *param, void *packet)
{
    (void)param;
    (void)packet;
}

/* 组装完成的 PS 包送进 demuxer。PACKET_LOST 标记的包（RTP 层判定丢帧）直接
 * 丢弃——把伪造的半包喂给 PS 解析会造成时间戳/帧边界错乱，不如等下一个完整包。 */
static int rtp_ps_payload_packet(void *param, const void *packet, int bytes, uint32_t timestamp,
                                 int flags)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)param;

    (void)timestamp;
    if (!slot || !slot->ps_demuxer || !packet || bytes <= 0) {
        return 0;
    }
    if (flags & RTP_PAYLOAD_FLAG_PACKET_LOST) {
        return 0;
    }
    return zms_mpegps_demuxer_feed(slot->ps_demuxer, (const uint8_t *)packet, (size_t)bytes) ==
                   ZN_OK
               ? 0
               : -1;
}

/*
 * 建媒体端点：ingress（复用统一推流管道）+ PS demuxer + RTP 载荷解码器。
 *
 * 几处 clock/timeline 设定是为 PS 特性服务的：
 *   - video/audio 时钟都设 90000：PS 的 PES 时间戳就是 90k 基准；
 *   - set_stamp_max_delta(10000)：容忍 10s 的跳变，避免设备重启/回绕时被判为
 *     异常而丢帧；
 *   - defer_gop_vcfg=1：视频配置延后到真正拿到 SPS/PPS 再落地（PS 里 config
 *     可能与首帧分离）；timeline 线性化保证音视频按 DTS 单调推进。
 */
static zn_err_t rtp_ps_slot_create_media(zms_rtp_ps_server_slot *slot, const char *app,
                                          const char *stream)
{
    zms_mpegps_demuxer_opts psopts;
    struct rtp_payload_t rtp_handler;

    if (!slot || !app || !app[0] || !stream || !stream[0]) {
        return ZN_ERR_INVALID;
    }

    slot->ingress = zms_live_ingest_create_publish_schema_vhost(ZMS_SCHEMA_RTP_PS, slot->vhost,
                                                                 app, stream, NULL);
    if (slot->ingress) {
        zms_live_ingest_set_poller(slot->ingress, slot->poller);
    }
    slot->source = slot->ingress ? zms_live_ingest_source(slot->ingress) : NULL;
    if (!slot->source) {
        return ZN_ERR_STATE;
    }

    zms_live_ingest_set_rtp_clocks(slot->ingress, 90000, ZMS_CODEC_AAC, 90000);
    zms_live_ingest_set_stamp_max_delta(slot->ingress, 10000);
    zms_live_ingest_set_stamp_av_clamp(slot->ingress, 0);
    zms_live_ingest_set_timeline_linear_ms(slot->ingress, 1);
    zms_live_ingest_set_defer_gop_vcfg(slot->ingress, 1);

    memset(&psopts, 0, sizeof(psopts));
    psopts.on_frame = rtp_ps_on_ps_frame;
    psopts.on_h264_ps = rtp_ps_on_h264_ps;
    psopts.user = slot;
    slot->ps_demuxer = zms_mpegps_demuxer_create(&psopts);
    if (!slot->ps_demuxer) {
        zms_live_ingest_destroy(slot->ingress);
        slot->ingress = NULL;
        slot->source = NULL;
        return ZN_ERR_NOMEM;
    }

    memset(&rtp_handler, 0, sizeof(rtp_handler));
    rtp_handler.alloc = rtp_ps_payload_alloc;
    rtp_handler.free = rtp_ps_payload_free;
    rtp_handler.packet = rtp_ps_payload_packet;
    slot->rtp_ps_decoder = rtp_payload_decode_create(slot->payload_type, "PS", &rtp_handler, slot);
    if (!slot->rtp_ps_decoder) {
        zms_mpegps_demuxer_destroy(slot->ps_demuxer);
        slot->ps_demuxer = NULL;
        zms_live_ingest_destroy(slot->ingress);
        slot->ingress = NULL;
        slot->source = NULL;
        return ZN_ERR_NOMEM;
    }

    return ZN_OK;
}

/*
 * 拆卸媒体端点。linger 判断是重点：若媒体源处于“linger”（刚断流但保留一小段
 * 供重连/观看的宽限期），就不要 reset ingress、也不要 publish_fini，否则会把
 * 仍在被其他订阅者使用的源强行清空。只有非 linger 才真正销毁并广播下线。
 */
static void rtp_ps_slot_teardown_media(zms_rtp_ps_server_slot *slot)
{
    zms_media_source *src;

    if (!slot) {
        return;
    }
    src = slot->source;
    if (slot->rtp_ps_decoder) {
        rtp_payload_decode_destroy(slot->rtp_ps_decoder);
        slot->rtp_ps_decoder = NULL;
    }
    if (slot->ps_demuxer) {
        zms_mpegps_demuxer_destroy(slot->ps_demuxer);
        slot->ps_demuxer = NULL;
    }
    zms_buf_pool_slot_clear(&slot->rtp_ps_assembly, &slot->rtp_ps_assembly_cap);
    {
        int linger = src ? zms_media_source_linger(src, ZMS_ORIGIN_RTP_PS_PUSH) : 0;

        if (!linger && slot->ingress) {
            zms_live_ingest_reset(slot->ingress);
        }
        if (src) {
            zms_media_source_clear_publisher(src, slot);
            if (!linger) {
                zms_media_event_publish_fini(src, ZMS_ORIGIN_RTP_PS_PUSH);
            }
        }
    }
    if (slot->ingress) {
        zms_live_ingest_destroy(slot->ingress);
        slot->ingress = NULL;
    }
    slot->source = NULL;
    slot->publish_started = 0;
}

/*
 * 过滤并送一包 RTP 到 PS 解复用器。
 * 两道丢弃：payload type 不符、SSRC 不符（可选）。GB28181 场景常有多路流
 * 打到同一端口或存在探测包，靠 pt/ssrc 精确过滤掉非本会话的杂包。
 * 丢弃日志只在前几包（recv_pkts<=3）打印：错配若持续发生，每包一条日志会
 * 淹没输出；前几包足以暴露问题。
 */
static void rtp_ps_feed_rtp_packet(zms_rtp_ps_server_slot *slot, const uint8_t *data, size_t len,
                                   const char *peer_ip, uint16_t peer_port, int is_first_raw)
{
    zms_rtp_packet rtp;

    if (!slot || !data || len < ZMS_RTP_HDR_SIZE) {
        return;
    }
    if (is_first_raw) {
        zn_debug(
            "[GB28181 zms 4/6] first RTP/PS TCP from %s:%u len=%zu app=%s stream=%s expect_pt=%d",
            peer_ip ? peer_ip : "?", (unsigned)peer_port, len, slot->app, slot->stream,
            slot->payload_type);
    }
    if (zms_rtp_is_rtcp(data, len)) {
        return;
    }
    if (zms_rtp_parse(data, len, &rtp) != ZN_OK) {
        return;
    }
    if (slot->payload_type >= 0 && rtp.hdr.pt != (uint8_t)slot->payload_type) {
        if (slot->recv_pkts <= 3) {
            zn_debug("[GB28181 zms] drop RTP pt=%u expect=%d app=%s stream=%s",
                      (unsigned)rtp.hdr.pt, slot->payload_type, slot->app, slot->stream);
        }
        return;
    }
    if (slot->enable_ssrc_filter && rtp.hdr.ssrc != slot->ssrc_filter) {
        if (slot->recv_pkts <= 3) {
            zn_debug("[GB28181 zms] drop RTP ssrc=%u filter=%u app=%s stream=%s",
                      (unsigned)rtp.hdr.ssrc, (unsigned)slot->ssrc_filter, slot->app, slot->stream);
        }
        return;
    }
    if (!slot->rtp_ps_decoder) {
        return;
    }
    slot->recv_pkts++;
    if (slot->recv_pkts == 1) {
        zn_info("[GB28181 zms 5/6] first RTP/PS app=%s stream=%s pt=%u ssrc=%u len=%zu from %s:%u "
                 "tcp_mode=%d",
                 slot->app, slot->stream, (unsigned)rtp.hdr.pt, (unsigned)rtp.hdr.ssrc, len,
                 peer_ip ? peer_ip : "?", (unsigned)peer_port, slot->tcp_mode);
    }
    (void)rtp_payload_decode_input(slot->rtp_ps_decoder, data, (int)len);
}

static int rtp_ps_tcp_asm_grow(zms_rtp_ps_server_slot *slot, size_t need)
{
    size_t cap;
    uint8_t *p;

    if (!slot) {
        return 0;
    }
    cap = slot->tcp_asm_cap ? slot->tcp_asm_cap : ZMS_RTP_PS_TCP_ASM_INIT;
    while (cap < need) {
        cap *= 2;
    }
    if (cap <= slot->tcp_asm_cap) {
        return 1;
    }
    p = (uint8_t *)realloc(slot->tcp_asm, cap);
    if (!p) {
        return 0;
    }
    slot->tcp_asm = p;
    slot->tcp_asm_cap = cap;
    return 1;
}

/** RFC4571：16 位 BE 长度 + RTP 包（GB28181 TCP/RTP/AVP） */
/* TCP 上 RTP 是定长前缀帧，且会跨 recv 边界，故必须累积到 tcp_asm 再按长度
 * 一帧帧切。长度字段非法（< RTP 头或 > 65535）时无法信任后续字节，直接清空
 * 缓冲重同步——继续按坏长度解析只会连环错位。 */
static void rtp_ps_tcp_feed(zms_rtp_ps_server_slot *slot, const void *data, size_t len,
                            const char *peer_ip, uint16_t peer_port)
{
    const uint8_t *in = (const uint8_t *)data;
    size_t off = 0;
    int first_raw = (slot->recv_pkts == 0 && slot->raw_udp_pkts == 0);

    if (!slot || !data || len == 0) {
        return;
    }
    slot->raw_udp_pkts++;
    if (!rtp_ps_tcp_asm_grow(slot, slot->tcp_asm_len + len)) {
        return;
    }
    memcpy(slot->tcp_asm + slot->tcp_asm_len, in, len);
    slot->tcp_asm_len += len;

    for (;;) {
        size_t remain = slot->tcp_asm_len;
        uint16_t pkt_len;

        if (remain < 2) {
            break;
        }
        pkt_len = (uint16_t)(((uint16_t)slot->tcp_asm[0] << 8) | slot->tcp_asm[1]);
        if (pkt_len < ZMS_RTP_HDR_SIZE || pkt_len > 65535) {
            zn_warn("[GB28181 zms] bad TCP RTP length=%u app=%s stream=%s, resync",
                     (unsigned)pkt_len, slot->app, slot->stream);
            slot->tcp_asm_len = 0;
            break;
        }
        if (remain < (size_t)2 + pkt_len) {
            break;
        }
        rtp_ps_feed_rtp_packet(slot, slot->tcp_asm + 2, pkt_len, peer_ip, peer_port, first_raw);
        first_raw = 0;
        memmove(slot->tcp_asm, slot->tcp_asm + 2 + pkt_len, remain - 2 - pkt_len);
        slot->tcp_asm_len = remain - 2 - pkt_len;
    }
}

static void rtp_ps_on_udp_packet(zn_udp_server *srv, const char *peer_ip, uint16_t peer_port,
                                 const void *data, size_t len, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;

    (void)srv;
    if (!slot || !data || len < ZMS_RTP_HDR_SIZE) {
        return;
    }
    slot->raw_udp_pkts++;
    if (slot->raw_udp_pkts == 1) {
        zn_debug("[GB28181 zms 4/6] first UDP from %s:%u len=%zu app=%s stream=%s expect_pt=%d "
                  "ssrc_filter=%s",
                  peer_ip ? peer_ip : "?", (unsigned)peer_port, len, slot->app, slot->stream,
                  slot->payload_type, slot->enable_ssrc_filter ? "on" : "off");
    }
    rtp_ps_feed_rtp_packet(slot, (const uint8_t *)data, len, peer_ip, peer_port, 0);
}

static void rtp_ps_tcp_session_close(zms_rtp_ps_server_slot *slot)
{
    if (!slot) {
        return;
    }
    if (slot->tcp_sess) {
        zn_tcp_session_close(slot->tcp_sess);
        slot->tcp_sess = NULL;
    }
    if (slot->tcp_cli) {
        zn_tcp_client_close(slot->tcp_cli);
        zn_tcp_client_destroy(slot->tcp_cli);
        slot->tcp_cli = NULL;
    }
    slot->tcp_asm_len = 0;
}

static void rtp_ps_tcp_on_recv(zn_tcp_session *session, const void *data, size_t len, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)session;
    if (!slot) {
        return;
    }
    rtp_ps_tcp_feed(slot, data, len, slot->tcp_peer_ip[0] ? slot->tcp_peer_ip : "?",
                    slot->tcp_peer_port ? slot->tcp_peer_port : 0);
}

static void rtp_ps_tcp_on_error(zn_tcp_session *session, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)session;
    if (!slot) {
        return;
    }
    zn_debug("[GB28181 zms] RTP/PS TCP session closed app=%s stream=%s", slot->app, slot->stream);
    slot->tcp_sess = NULL;
}

static void rtp_ps_tcp_client_on_connect(zn_tcp_client *client, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)client;
    if (!slot) {
        return;
    }
    zn_debug("[GB28181 zms 3/6] TCP-ACTIVE connected -> %s:%u app=%s stream=%s", slot->tcp_peer_ip,
              (unsigned)slot->tcp_peer_port, slot->app, slot->stream);
}

static void rtp_ps_tcp_client_on_recv(zn_tcp_client *client, const void *data, size_t len,
                                      void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)client;
    if (!slot) {
        return;
    }
    rtp_ps_tcp_feed(slot, data, len, slot->tcp_peer_ip, slot->tcp_peer_port);
}

static void rtp_ps_tcp_client_on_error(zn_tcp_client *client, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    (void)client;
    if (!slot) {
        return;
    }
    zn_warn("[GB28181 zms] TCP-ACTIVE error app=%s stream=%s -> %s:%u", slot->app, slot->stream,
             slot->tcp_peer_ip, (unsigned)slot->tcp_peer_port);
    slot->tcp_cli = NULL;
}

static void rtp_ps_tcp_passive_on_recv(zn_tcp_session *session, const void *data, size_t len,
                                       void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    zn_socket *sock;
    char peer_ip[64];
    uint16_t peer_port = 0;

    if (!slot) {
        return;
    }
    /* 每个槽位只接受第一条被 accept 的连接：GB28181 一个媒体通道对应一个
     * 发送端，多余连接若不是同一条 session 就忽略，防止误接到别的推流者。 */
    if (slot->tcp_sess && slot->tcp_sess != session) {
        return;
    }
    if (!slot->tcp_sess) {
        slot->tcp_sess = session;
        sock = zn_tcp_session_socket(session);
        if (sock) {
            (void)zn_socket_get_peer(sock, peer_ip, sizeof(peer_ip), &peer_port);
        }
        strncpy(slot->tcp_peer_ip, peer_ip, sizeof(slot->tcp_peer_ip) - 1);
        slot->tcp_peer_port = peer_port;
        zn_debug(
            "[GB28181 zms 3/6] TCP-PASSIVE accepted from %s:%u app=%s stream=%s listen_port=%u",
            peer_ip, (unsigned)peer_port, slot->app, slot->stream, (unsigned)slot->port);
    }
    rtp_ps_tcp_on_recv(session, data, len, slot);
}

static void rtp_ps_tcp_passive_on_error(zn_tcp_session *session, void *user)
{
    zms_rtp_ps_server_slot *slot = (zms_rtp_ps_server_slot *)user;
    if (!slot) {
        return;
    }
    if (slot->tcp_sess == session) {
        slot->tcp_sess = NULL;
    }
    rtp_ps_tcp_on_error(session, slot);
}

/*
 * 为 TCP-ACTIVE 模式“预约”一个空闲 UDP 端口号：临时 bind 一个端口拿到系统
 * 分配的端口号后立即释放，把号码告诉平台（我们随后作为 client 主动连过去）。
 * 这里其实不占用该端口——TCP 主动连接不需要监听，只需要一个对外公告的
 * 端口号（部分平台会据此校验）。用 UDP 探测只是借用内核的端口分配机制，
 * reuse=1 降低与真占用的冲突。
 */
static zn_err_t rtp_ps_reserve_port_udp(uint16_t want_port, zn_poller *poller, uint16_t *out_port)
{
    zn_udp_server_opts_t uopts;
    static const zn_udp_server_ops_t noop_ops = {NULL};
    zn_udp_server *udp;

    memset(&uopts, 0, sizeof(uopts));
    uopts.host = "0.0.0.0";
    uopts.port = want_port;
    uopts.reuse = 1;
    uopts.poller = poller;
    uopts.ops = &noop_ops;
    udp = zn_udp_server_create(&uopts);
    if (!udp || zn_udp_server_start(udp) != ZN_OK) {
        if (udp) {
            zn_udp_server_destroy(udp);
        }
        return ZN_ERR_STATE;
    }
    *out_port = zn_udp_server_port(udp);
    zn_udp_server_stop(udp);
    zn_udp_server_destroy(udp);
    return ZN_OK;
}

static zn_err_t rtp_ps_tcp_passive_listen(zms_rtp_ps_server_slot *slot, uint16_t port)
{
    static const zn_tcp_session_ops_t passive_ops = {
        rtp_ps_tcp_passive_on_recv,
        rtp_ps_tcp_passive_on_error,
        NULL,
    };
    zn_tcp_server_opts_t topts;

    if (!slot || !slot->poller_pool) {
        return ZN_ERR_INVALID;
    }
    memset(&topts, 0, sizeof(topts));
    topts.host = "0.0.0.0";
    topts.port = port;
    topts.backlog = 8;
    topts.poller_pool = slot->poller_pool;
    topts.session_ops = &passive_ops;
    topts.session_user = slot;
    slot->tcp_srv = zn_tcp_server_create(&topts);
    if (!slot->tcp_srv || zn_tcp_server_start(slot->tcp_srv) != ZN_OK) {
        if (slot->tcp_srv) {
            zn_tcp_server_destroy(slot->tcp_srv);
            slot->tcp_srv = NULL;
        }
        return ZN_ERR_STATE;
    }
    slot->port = zn_tcp_server_port(slot->tcp_srv);
    return ZN_OK;
}

static zn_err_t rtp_ps_udp_listen(zms_rtp_ps_server_slot *slot, uint16_t port)
{
    zn_udp_server_opts_t uopts;
    static const zn_udp_server_ops_t udp_ops = {rtp_ps_on_udp_packet};

    memset(&uopts, 0, sizeof(uopts));
    uopts.host = "0.0.0.0";
    uopts.port = port;
    uopts.reuse = 1;
    uopts.poller = slot->poller;
    uopts.ops = &udp_ops;
    uopts.user = slot;
    slot->udp = zn_udp_server_create(&uopts);
    if (!slot->udp || zn_udp_server_start(slot->udp) != ZN_OK) {
        return ZN_ERR_STATE;
    }
    slot->port = zn_udp_server_port(slot->udp);
    return ZN_OK;
}

static void rtp_ps_slot_destroy(zms_rtp_ps_server_slot *slot)
{
    if (!slot) {
        return;
    }
    rtp_ps_reg_remove(slot);
    rtp_ps_tcp_session_close(slot);
    if (slot->tcp_srv) {
        zn_tcp_server_stop(slot->tcp_srv);
        zn_tcp_server_destroy(slot->tcp_srv);
        slot->tcp_srv = NULL;
    }
    if (slot->udp) {
        zn_udp_server_stop(slot->udp);
        zn_udp_server_destroy(slot->udp);
        slot->udp = NULL;
    }
    free(slot->tcp_asm);
    slot->tcp_asm = NULL;
    slot->tcp_asm_cap = slot->tcp_asm_len = 0;
    rtp_ps_slot_teardown_media(slot);
    free(slot);
}

/* 关闭并释放槽位（对外 API）。关闭顺序在 rtp_ps_slot_destroy 内固定为：
 * 先摘注册表 → 断连接 → 停监听 → 释放装配缓冲 → 拆媒体端点 → free。 */
void zms_rtp_ps_server_close(zms_rtp_ps_server_slot *slot)
{
    rtp_ps_slot_destroy(slot);
}

zn_err_t zms_rtp_ps_server_connect(zn_poller *poller, const char *vhost, const char *app,
                                    const char *stream, const char *host, uint16_t port)
{
    zms_rtp_ps_server_slot *slot;
    char key[256];
    zn_tcp_client_opts_t copts;
    static const zn_tcp_client_ops_t cli_ops = {
        rtp_ps_tcp_client_on_connect,
        rtp_ps_tcp_client_on_recv,
        rtp_ps_tcp_client_on_error,
    };

    if (!poller || !app || !stream || !host || !host[0] || port == 0) {
        return ZN_ERR_INVALID;
    }
    zms_rtp_ps_server_make_key(vhost, app, stream, key, sizeof(key));
    slot = zms_rtp_ps_server_find_by_key(key);
    if (!slot) {
        return ZN_ERR_INVALID;
    }
    if (slot->tcp_mode != ZMS_RTP_PS_TCP_ACTIVE) {
        return ZN_ERR_INVALID;
    }
    if (slot->tcp_cli || slot->tcp_sess) {
        return ZN_ERR_STATE;
    }

    {
        char ipbuf[64];

    strncpy(slot->tcp_peer_ip, host, sizeof(slot->tcp_peer_ip) - 1);
    slot->tcp_peer_port = port;
    slot->poller = poller;

    memset(&copts, 0, sizeof(copts));
    copts.poller = poller;
    copts.ops = &cli_ops;
    copts.user = slot;
    slot->tcp_cli = zn_tcp_client_create(&copts);
    if (!slot->tcp_cli) {
        return ZN_ERR_NOMEM;
    }
    zn_debug("[GB28181 zms 3/6] TCP-ACTIVE connecting -> %s:%u app=%s stream=%s", host,
              (unsigned)port, app, stream);
    if (zn_tcp_client_connect(slot->tcp_cli, zms_dns_target(host, ipbuf, sizeof(ipbuf)), port) !=
        ZN_OK) {
        zn_tcp_client_destroy(slot->tcp_cli);
        slot->tcp_cli = NULL;
        return ZN_ERR_IO;
    }
    }
    return ZN_OK;
}

zn_err_t zms_rtp_ps_server_open(const zms_rtp_ps_server_open_opts *opts,
                                 zms_rtp_ps_server_slot **out_slot, uint16_t *out_port)
{
    zms_rtp_ps_server_slot *slot;
    zms_rtp_ps_server_open_opts local;
    const char *app;
    const char *stream;
    char key[256];
    zms_rtp_ps_server_slot *old;
    zms_auth_decision auth_dec;
    zn_err_t err;
    const char *mode_name;

    if (!opts || !opts->poller || !out_slot) {
        return ZN_ERR_INVALID;
    }
    local = *opts;
    app = local.app && local.app[0] ? local.app : "live";
    stream = local.stream && local.stream[0] ? local.stream : NULL;
    if (!stream) {
        return ZN_ERR_INVALID;
    }
    if (local.tcp_mode < 0) {
        local.tcp_mode = ZMS_RTP_PS_TCP_UDP;
    }
    if (local.tcp_mode == ZMS_RTP_PS_TCP_PASSIVE && !local.poller_pool) {
        return ZN_ERR_INVALID;
    }

    memset(&auth_dec, 0, sizeof(auth_dec));
    {
        zms_auth_request auth_req;

        memset(&auth_req, 0, sizeof(auth_req));
        auth_req.action = ZMS_AUTH_ACTION_PUBLISH;
        auth_req.schema = ZMS_SCHEMA_RTP_PS;
        auth_req.vhost = local.vhost;
        auth_req.app = app;
        auth_req.stream = stream;
        auth_req.params = local.params;
        if (!zms_auth_check(&auth_req, &auth_dec)) {
            zn_warn("[rtp-ps] open denied: app=%s stream=%s", app, stream);
            return ZN_ERR_DENIED;
        }
    }

    zms_rtp_ps_server_make_key(local.vhost, app, stream, key, sizeof(key));
    old = zms_rtp_ps_server_find_by_key(key);
    if (old) {
        /* 双击点播/重复 openRtpServer：复用已有监听，避免 close+重建竞态崩溃 */
        if (old->tcp_mode == local.tcp_mode && old->port != 0 &&
            (local.port == 0 || local.port == old->port)) {
            zn_info("[GB28181 zms 2/6] openRtpServer reuse key=%s port=%u pt=%d tcp_mode=%d "
                     "publish=%d",
                     key, (unsigned)old->port, old->payload_type, old->tcp_mode,
                     old->publish_started);
            *out_slot = old;
            if (out_port) {
                *out_port = old->port;
            }
            return ZN_OK;
        }
        zn_info("[GB28181 zms 2/6] openRtpServer replace key=%s old_port=%u -> new (mode %d->%d)",
                 key, (unsigned)old->port, old->tcp_mode, local.tcp_mode);
        zms_rtp_ps_server_close(old);
    }

    slot = (zms_rtp_ps_server_slot *)calloc(1, sizeof(*slot));
    if (!slot) {
        return ZN_ERR_NOMEM;
    }
    strncpy(slot->key, key, sizeof(slot->key) - 1);
    zms_vhost_normalize(local.vhost, slot->vhost, sizeof(slot->vhost));
    strncpy(slot->app, app, sizeof(slot->app) - 1);
    strncpy(slot->stream, stream, sizeof(slot->stream) - 1);
    slot->poller = local.poller;
    slot->poller_pool = local.poller_pool;
    slot->tcp_mode = local.tcp_mode;
    slot->payload_type = local.payload_type > 0 ? local.payload_type : ZMS_RTP_PS_DEFAULT_PT;
    slot->ssrc_filter = local.ssrc;
    slot->enable_ssrc_filter = local.enable_ssrc_filter;
    slot->signaling_authorized = local.signaling_authorized ? 1 : 0;
    slot->auth_enable_mp4 = auth_dec.enable_mp4;

    if (rtp_ps_slot_create_media(slot, app, stream) != ZN_OK) {
        free(slot);
        return ZN_ERR_STATE;
    }

    switch (slot->tcp_mode) {
    case ZMS_RTP_PS_TCP_PASSIVE:
        err = rtp_ps_tcp_passive_listen(slot, local.port);
        mode_name = "TCP-PASSIVE listen";
        break;
    case ZMS_RTP_PS_TCP_ACTIVE:
        err = rtp_ps_reserve_port_udp(local.port, slot->poller, &slot->port);
        mode_name = "TCP-ACTIVE (await connectRtpServer)";
        break;
    default:
        err = rtp_ps_udp_listen(slot, local.port);
        mode_name = "UDP";
        slot->tcp_mode = ZMS_RTP_PS_TCP_UDP;
        break;
    }
    if (err != ZN_OK) {
        rtp_ps_slot_destroy(slot);
        return err;
    }

    rtp_ps_reg_add(slot);
    ++g_rtp_ps_session_no;
    zn_info("[GB28181 zms 2/6] openRtpServer #%u key=%s port=%u pt=%d mode=%s ssrc_filter=%s",
             g_rtp_ps_session_no, key, (unsigned)slot->port, slot->payload_type, mode_name,
             local.enable_ssrc_filter ? "on" : "off");

    *out_slot = slot;
    if (out_port) {
        *out_port = slot->port;
    }
    return ZN_OK;
}
