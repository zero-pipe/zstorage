/**
 * @file signaling_client.c
 * @brief 外部 WebSocket 信令客户端（设备 master）：
 *        join -> 收 viewer offer -> 复用 WHEP 会话核心 answer -> trickle candidate。
 *
 * 信令协议与 ipc-mini cloud/signaling 对齐：
 *   in:  joined / peer-joined / peer-left / offer / answer / candidate / ping
 *   out: join / offer / answer / candidate / leave / ping
 */
#include "session/webrtc/signaling_client.h"
#include "zms/protocol/http/websocket/framer.h"
#include "zms/util/base64.h"

#include "protocol/webrtc/ice_internal.h"
#include "session/webrtc/session_internal.h"
#include "zms/core/event.h"
#include "zms/core/stream/hub.h"
#include "zms/egress/source/egress_source.h"
#include "zms/play/dispatch.h"
#include "zms/session/webrtc/webrtc_server.h"
#include "zms/util/buf_pool.h"
#include "zn/net/tcp_client.h"
#include "zn/poller/poller_pool.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 信令连接是进程级单例（设备作为 master 接入一个房间），因此用 g_sig 保存，
 * 不做多实例管理。SIG_MAX_VIEWERS=3 是对“一个房间向本设备拉流的人数”的
 * 硬上限，避免单个 master 被无限拉流拖垮。 */
#define SIG_HS_MAX 4096
#define SIG_RX_MAX 65536
#define SIG_TX_MAX 65536
#define SIG_RETRY_MS 2000
#define SIG_MAX_VIEWERS 3

typedef struct sig_viewer {
    zms_webrtc_session *sess;
    char viewer_id[64];
} sig_viewer;

typedef struct zms_webrtc_signaling_ctx {
    zn_poller_pool *pool;
    zn_poller *poller;
    zn_tcp_client *tcp;
    char host[128];
    char path[256];
    char token[128];
    char room[64];
    char app[64];
    char stream[128];
    uint16_t port;
    int stop;
    int ws_open;
    int hs_done;
    int retry_scheduled;
    char hs[SIG_HS_MAX];
    size_t hs_len;
    uint8_t rx[SIG_RX_MAX];
    size_t rx_len;
    sig_viewer viewers[SIG_MAX_VIEWERS];
    char client_id[48];
} sig_ctx;

static sig_ctx *g_sig;

/* 查找 viewer。id 为空时返回第一个在用的 viewer——用于某些服务端消息不带
 * 来源 id 的场景（单观众时语义明确；多观众时这是尽力而为的兜底）。 */
static sig_viewer *sig_find_viewer(sig_ctx *c, const char *id)
{
    int i;

    if (!c) {
        return NULL;
    }
    for (i = 0; i < SIG_MAX_VIEWERS; ++i) {
        if (!c->viewers[i].sess) {
            continue;
        }
        if (!id || !id[0] || strcmp(c->viewers[i].viewer_id, id) == 0) {
            return &c->viewers[i];
        }
    }
    return NULL;
}

static sig_viewer *sig_alloc_viewer(sig_ctx *c)
{
    int i;

    if (!c) {
        return NULL;
    }
    for (i = 0; i < SIG_MAX_VIEWERS; ++i) {
        if (!c->viewers[i].sess) {
            return &c->viewers[i];
        }
    }
    return NULL;
}

static void sig_destroy_viewer(sig_ctx *c, sig_viewer *v)
{
    if (!c || !v) {
        return;
    }
    if (v->sess) {
        zms_webrtc_session_destroy(v->sess);
        v->sess = NULL;
    }
    v->viewer_id[0] = '\0';
}

static void sig_destroy_session(sig_ctx *c, const char *viewer)
{
    sig_destroy_viewer(c, sig_find_viewer(c, viewer));
}

/* 销毁本端持有的全部 viewer 会话（断链/停止时用）。逐个 destroy_viewer 释放
 * WebRTC 会话与其 egress，并清空槽位供后续复用。 */
static void sig_destroy_all(sig_ctx *c)
{
    int i;

    if (!c) {
        return;
    }
    for (i = 0; i < SIG_MAX_VIEWERS; ++i) {
        sig_destroy_viewer(c, &c->viewers[i]);
    }
}

/* ── JSON（固定消息集，不做通用解析） ─────────────────────────────────── */

static const char *json_value(const char *json, const char *key)
{
    char pat[96];
    const char *p;

    if (!json || !key) {
        return NULL;
    }
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != ':') {
        return NULL;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static int json_string(const char *json, const char *key, char *out, size_t cap)
{
    const char *p = json_value(json, key);
    size_t n = 0;

    if (!p || !out || cap == 0 || *p != '"') {
        return -1;
    }
    ++p;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\') {
            ++p;
            switch (*p) {
            case 'n':
                out[n++] = '\n';
                break;
            case 'r':
                out[n++] = '\r';
                break;
            case 't':
                out[n++] = '\t';
                break;
            case 'b':
                out[n++] = '\b';
                break;
            case 'f':
                out[n++] = '\f';
                break;
            case '"':
            case '\\':
            case '/':
                out[n++] = *p;
                break;
            default:
                if (*p) {
                    out[n++] = *p;
                }
                break;
            }
            if (*p) {
                ++p;
            }
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = '\0';
    return 0;
}

static size_t json_escape(char *out, size_t cap, const char *s)
{
    size_t n = 0;

    if (!out || cap == 0) {
        return 0;
    }
    for (; s && *s && n + 1 < cap; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') {
            if (n + 2 >= cap) {
                break;
            }
            out[n++] = '\\';
            out[n++] = (char)ch;
        } else if (ch == '\r') {
            if (n + 2 >= cap) {
                break;
            }
            out[n++] = '\\';
            out[n++] = 'r';
        } else if (ch == '\n') {
            if (n + 2 >= cap) {
                break;
            }
            out[n++] = '\\';
            out[n++] = 'n';
        } else if (ch < 0x20) {
            if (n + 1 < cap) {
                out[n++] = ' ';
            }
        } else {
            out[n++] = (char)ch;
        }
    }
    out[n] = '\0';
    return n;
}

/* ── WebSocket 客户端 ─────────────────────────────────────────────────── */

static int sig_send_raw(sig_ctx *c, const void *data, size_t len)
{
    if (!c || !c->tcp || !data || len == 0) {
        return -1;
    }
    return zn_tcp_client_send(c->tcp, data, len) == ZN_OK ? 0 : -1;
}

/* 发送一个带掩码的 WebSocket 帧。RFC 6455 要求客户端发出的帧必须加掩码
 * （mask key 随机），服务端会据此校验；这里用一个固定 mask 仅是内部链路，
 * 不追求掩码随机性（掩码的作用是防代理缓存投毒，此处无该威胁）。
 * 固定帧头 + 负载整体 malloc 后一次 send，避免拆包写出半帧。 */
static int ws_send_frame(sig_ctx *c, uint8_t opcode, const void *payload, size_t len)
{
    uint8_t hdr[ZMS_WS_FRAMER_CLIENT_HDR_MAX];
    uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t *frame;
    size_t hlen;
    int rc;

    if (!c || len > SIG_TX_MAX) {
        return -1;
    }
    hlen = zms_ws_framer_client_hdr(hdr, sizeof(hdr), opcode, mask, len);
    if (hlen == 0) {
        return -1;
    }
    frame = (uint8_t *)malloc(hlen + len);
    if (!frame) {
        return -1;
    }
    memcpy(frame, hdr, hlen);
    if (len > 0) {
        memcpy(frame + hlen, payload, len);
        zms_ws_framer_apply_mask(frame + hlen, len, mask);
    }
    rc = sig_send_raw(c, frame, hlen + len);
    free(frame);
    return rc;
}

static int sig_send_json(sig_ctx *c, const char *json)
{
    return ws_send_frame(c, 0x1, json, strlen(json));
}

static uint64_t sig_retry_cb(void *user)
{
    sig_ctx *c = (sig_ctx *)user;

    c->retry_scheduled = 0;
    if (!c->stop) {
        (void)zn_tcp_client_connect(c->tcp, c->host, c->port);
    }
    return 0;
}

/* 安排一次重连（延时 SIG_RETRY_MS）。retry_scheduled 去重：连接失败与错误
 * 回调可能在短时间内都触发，若不去重会排出一串重复的定时 connect，造成
 * 连接风暴。stop 后不再重连。 */
static void sig_schedule_retry(sig_ctx *c)
{
    if (!c || c->stop || c->retry_scheduled || !c->poller) {
        return;
    }
    c->retry_scheduled = 1;
    (void)zn_poller_do_delay(c->poller, SIG_RETRY_MS, sig_retry_cb, c);
}

/* 加入房间：role=master 表示本端是“被拉流的设备方”（由 viewer 发 offer）。
 * clientId 用于平台区分同房间内多个 master，重连后保持一致以便识别。 */
static void sig_send_join(sig_ctx *c)
{
    char msg[512];

    snprintf(msg, sizeof(msg),
             "{\"type\":\"join\",\"room\":\"%s\",\"role\":\"master\",\"clientId\":\"%s\"}",
             c->room, c->client_id);
    (void)sig_send_json(c, msg);
    zn_info("[webrtc-signal] join room=%s client=%s", c->room, c->client_id);
}

static void sig_send_candidates_from_sdp(sig_ctx *c, const char *sdp, const char *to)
{
    const char *p = sdp;
    char cand[512];
    char msg[768];

    while (p && (p = strstr(p, "a=candidate:")) != NULL) {
        const char *end = strstr(p, "\r\n");
        size_t n = end ? (size_t)(end - p - 2) : strlen(p + 2);

        if (n >= sizeof(cand)) {
            n = sizeof(cand) - 1;
        }
        memcpy(cand, p + 2, n);
        cand[n] = '\0';
        snprintf(msg, sizeof(msg),
                 "{\"type\":\"candidate\",\"to\":\"%s\",\"candidate\":\"%s\",\"sdpMid\":\"0\","
                 "\"sdpMLineIndex\":0}",
                 to, cand);
        (void)sig_send_json(c, msg);
        p = end ? end + 2 : p + strlen(p);
    }
}

static void sig_send_gathered_candidates(sig_ctx *c, sig_viewer *v)
{
    char cand[1024];
    char *line;
    char *next;

    if (!c || !v || !v->sess || !v->sess->ice_host.ctx || !v->viewer_id[0]) {
        return;
    }
    if (zms_webrtc_ice_sdp_candidates(v->sess->ice_host.ctx, cand, sizeof(cand)) == 0) {
        return;
    }
    line = cand;
    while (line && *line) {
        char msg[1280];
        next = strstr(line, "\r\n");
        if (next) {
            *next = '\0';
            next += 2;
        }
        if (strncmp(line, "a=", 2) == 0) {
            snprintf(msg, sizeof(msg),
                     "{\"type\":\"candidate\",\"to\":\"%s\",\"candidate\":\"%s\",\"sdpMid\":\"0\","
                     "\"sdpMLineIndex\":0}",
                     v->viewer_id, line + 2);
            (void)sig_send_json(c, msg);
        }
        line = next;
    }
}

typedef struct sig_gather_task {
    sig_ctx *c;
    char viewer_id[64];
} sig_gather_task;

static uint64_t sig_gather_cb(void *user)
{
    sig_gather_task *task = (sig_gather_task *)user;
    sig_viewer *v;

    if (task) {
        v = sig_find_viewer(task->c, task->viewer_id);
        if (v) {
            sig_send_gathered_candidates(task->c, v);
        }
        free(task);
    }
    return 0;
}

static void sig_log_offer_layout(const char *sdp)
{
    const char *p = sdp;

    while (p && *p) {
        const char *nl = strstr(p, "\r\n");
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if ((n > 2 && strncmp(p, "m=", 2) == 0) ||
            (n > 6 && strncmp(p, "a=mid:", 6) == 0) ||
            (n >= 14 && strncmp(p, "a=group:BUNDLE", 14) == 0)) {
            zn_info("[webrtc-signal] offer %.*s", (int)n, p);
        }
        p = nl ? nl + 2 : NULL;
    }
}

static int sig_send_answer(sig_ctx *c, const char *viewer, const char *answer, size_t answer_len)
{
    sig_gather_task *task;
    char *msg;
    char *esc;

    esc = (char *)malloc(answer_len * 2 + 16);
    msg = (char *)malloc(answer_len * 2 + 256);
    if (!esc || !msg) {
        free(esc);
        free(msg);
        return -1;
    }
    json_escape(esc, answer_len * 2 + 16, answer);
    snprintf(msg, answer_len * 2 + 256, "{\"type\":\"answer\",\"to\":\"%s\",\"sdp\":\"%s\"}",
             viewer, esc);
    free(esc);
    (void)sig_send_json(c, msg);
    free(msg);
    sig_send_candidates_from_sdp(c, answer, viewer);
    /* answer 里已带本地候选，但 ICE 仍在持续 gather（尤其 STUN/TURN）；
     * 500ms 后再补发一轮“新收集到的”候选做 trickle，兼顾首连及时与补充完整。 */
    task = (sig_gather_task *)calloc(1, sizeof(*task));
    if (task) {
        task->c = c;
        snprintf(task->viewer_id, sizeof(task->viewer_id), "%s", viewer);
        (void)zn_poller_do_delay(c->poller, 500, sig_gather_cb, task);
    }
    zn_info("[webrtc-signal] answer sent viewer=%s app=%s stream=%s", viewer, c->app, c->stream);
    return 0;
}

/*
 * 处理一个 viewer 的 offer，产出并回送 answer。
 *
 * 关键取舍——ICE restart 复用 vs 重建会话：
 *   若该 viewer 已有会话，且新 offer 的 ufrag 变了（对端重启了 ICE）但
 *   视频 payload type 没变，说明只是网络路径重协商，就地对现有会话做
 *   ICE restart 即可（保留已建好的 egress/管线，代价最小）；否则彻底销毁
 *   重建。probe_offer 先做轻量探测，避免对每个 offer 都重建会话。
 *
 * 注意：源查找只查一次，不等待——本函数在 poller 回调中执行，任何阻塞
 * （如等待源上线）都会卡住该 poller 上的全部会话。
 */
static int sig_create_answer(sig_ctx *c, const char *offer, const char *viewer)
{
    zms_media_source *src;
    zms_webrtc_session *sess;
    sig_viewer *v;
    char answer[8192];
    size_t answer_len = 0;
    zms_session_play_opts pcfg;

    src = NULL;
    /* 只查一次：这里是 poller 回调，sleep 等源上线会卡住整个事件循环。 */
    src = zms_media_source_find_for_play_ex(ZMS_SCHEMA_RTMP, NULL, c->app, c->stream);
    if (!src || !src->gop_queue || zms_media_source_is_vod(src) ||
        !zms_media_source_use_gop_queue_play(src)) {
        zn_warn("[webrtc-signal] source offline app=%s stream=%s", c->app, c->stream);
        return -1;
    }
    sig_log_offer_layout(offer);
    v = sig_find_viewer(c, viewer);
    if (v && v->sess) {
        char ufrag[64];
        int offer_pt = -1;

        if (zms_webrtc_session_probe_offer(offer, strlen(offer), ufrag, sizeof(ufrag), &offer_pt) ==
                0 &&
            ufrag[0] && strcmp(ufrag, v->sess->remote_ufrag) != 0 &&
            offer_pt == (int)v->sess->video_pt) {
            if (zms_webrtc_session_restart_ice(v->sess) == 0 &&
                zms_webrtc_session_build_answer(v->sess, offer, strlen(offer), answer,
                                                sizeof(answer), &answer_len) == 0) {
                zn_info("[webrtc-signal] ICE restart viewer=%s app=%s stream=%s", viewer, c->app,
                         c->stream);
                return sig_send_answer(c, viewer, answer, answer_len);
            }
            zn_warn("[webrtc-signal] ICE restart failed, recreate session viewer=%s", viewer);
        }
    }
    sig_destroy_session(c, viewer);
    v = sig_alloc_viewer(c);
    if (!v) {
        zn_warn("[webrtc-signal] viewer limit reached viewer=%s", viewer);
        return -1;
    }
    sess = zms_webrtc_session_create(src, c->app, c->stream, c->poller);
    if (!sess) {
        return -1;
    }
    zms_session_dispatch_register_all();
    zms_egress_source_init(&sess->play);
    memset(&pcfg, 0, sizeof(pcfg));
    pcfg.player = ZMS_SESSION_WEBRTC;
    if (zms_session_attach_play(ZMS_SESSION_WEBRTC, &sess->play, src, &pcfg) != ZN_OK) {
        zms_webrtc_session_destroy(sess);
        return -1;
    }
    if (zms_webrtc_session_build_answer(sess, offer, strlen(offer), answer, sizeof(answer),
                                        &answer_len) != 0) {
        zms_session_detach_play(ZMS_SESSION_WEBRTC, &sess->play);
        zms_webrtc_session_destroy(sess);
        return -1;
    }
    zms_media_source_reader_add(src);
    sess->play_reader_attached = 1;
    sess->play_start_ms = zn_monotonic_ms();
    zms_media_event_play(src, "webrtc-signal");
    v->sess = sess;
    snprintf(v->viewer_id, sizeof(v->viewer_id), "%s", viewer);

    return sig_send_answer(c, viewer, answer, answer_len);
}

/* 按 type 分发信令消息。未知 type 静默忽略（协议向前兼容：平台新增消息类型
 * 时老客户端不应报错崩溃）。offer/candidate/peer-left 会动会话状态，其余
 * （joined/peer-joined/ping）只记日志或回 pong。 */
static void sig_handle_message(sig_ctx *c, const char *json)
{
    char type[32];
    char from[64];
    /* SDP 实测 2~6KB；32KB 放在栈上没有必要，8KB 已远超真实报文。 */
    char sdp[8192];
    char cand[1024];

    if (json_string(json, "type", type, sizeof(type)) != 0) {
        return;
    }
    if (strcmp(type, "joined") == 0) {
        char id[64];
        json_string(json, "clientId", id, sizeof(id));
        zn_info("[webrtc-signal] joined client=%s", id);
        return;
    }
    if (strcmp(type, "peer-joined") == 0) {
        char id[64];
        char role[32];
        json_string(json, "clientId", id, sizeof(id));
        json_string(json, "role", role, sizeof(role));
        zn_info("[webrtc-signal] peer-joined role=%s client=%s", role, id);
        return;
    }
    if (strcmp(type, "offer") == 0) {
        if (json_string(json, "from", from, sizeof(from)) != 0 ||
            json_string(json, "sdp", sdp, sizeof(sdp)) != 0) {
            return;
        }
        (void)sig_create_answer(c, sdp, from);
        return;
    }
    if (strcmp(type, "candidate") == 0) {
        sig_viewer *v;

        if (json_string(json, "candidate", cand, sizeof(cand)) != 0) {
            return;
        }
        /* trickle 候选可能先于携带 from 的消息到达，缺 from 时按“唯一观众”
         * 兜底（sig_find_viewer 传 NULL）。候选需重新加 "a=" 前缀才能喂给
         * ICE 层的 SDP 解析。 */
        if (json_string(json, "from", from, sizeof(from)) != 0) {
            from[0] = '\0';
        }
        v = sig_find_viewer(c, from[0] ? from : NULL);
        if (v) {
            char frag[1152];
            snprintf(frag, sizeof(frag), "a=%s\r\n", cand);
            (void)zms_webrtc_session_add_remote_candidates(v->sess, frag, strlen(frag));
        }
        return;
    }
    if (strcmp(type, "peer-left") == 0) {
        if (json_string(json, "clientId", from, sizeof(from)) != 0 &&
            json_string(json, "from", from, sizeof(from)) != 0) {
            from[0] = '\0';
        }
        sig_destroy_session(c, from[0] ? from : NULL);
        return;
    }
    if (strcmp(type, "ping") == 0) {
        (void)sig_send_json(c, "{\"type\":\"pong\"}");
    }
}

static void sig_on_ws_text(sig_ctx *c, const char *text, size_t len)
{
    char *copy = (char *)malloc(len + 1);

    if (!copy) {
        return;
    }
    memcpy(copy, text, len);
    copy[len] = '\0';
    sig_handle_message(c, copy);
    free(copy);
}

/*
 * 从 rx 缓冲区里逐个拆 WebSocket 帧。
 *
 * 处理不完整帧的方式是 break（不消费），循环结束后把已消费的前缀 memmove
 * 到缓冲区头部，未到齐的尾部留待下次 recv 续上——这是流式解析必须的，
 * 不能假定一次 recv 恰好是一个完整帧。opcode：0x1 文本→业务消息；0x9 ping
 * →回 0xA pong；0x8 close→关连接并排定重连。收到 close 后立即 return，
 * 后续字节不再解析（连接已失效）。
 */
static void sig_parse_frames(sig_ctx *c)
{
    size_t off = 0;

    while (off + 2 <= c->rx_len) {
        const uint8_t *f = c->rx + off;
        uint8_t opcode = f[0] & 0x0f;
        int masked = (f[1] & 0x80) != 0;
        uint64_t plen = f[1] & 0x7f;
        size_t hlen = 2;
        size_t total;
        uint8_t mask[4] = {0};
        size_t i;

        if (plen == 126) {
            if (off + 4 > c->rx_len) {
                break;
            }
            plen = ((uint64_t)f[2] << 8) | f[3];
            hlen = 4;
        } else if (plen == 127) {
            if (off + 10 > c->rx_len) {
                break;
            }
            plen = 0;
            for (i = 0; i < 8; ++i) {
                plen = (plen << 8) | f[2 + i];
            }
            hlen = 10;
        }
        if (masked) {
            if (off + hlen + 4 > c->rx_len) {
                break;
            }
            memcpy(mask, f + hlen, 4);
            hlen += 4;
        }
        total = hlen + (size_t)plen;
        if (off + total > c->rx_len) {
            break;
        }
        if (opcode == 0x1) {
            if (masked) {
                char *text = (char *)malloc((size_t)plen + 1);
                if (text) {
                    for (i = 0; i < (size_t)plen; ++i) {
                        text[i] = (char)(f[hlen + i] ^ mask[i & 3]);
                    }
                    sig_on_ws_text(c, text, (size_t)plen);
                    free(text);
                }
            } else {
                sig_on_ws_text(c, (const char *)(f + hlen), (size_t)plen);
            }
        } else if (opcode == 0x9) {
            (void)ws_send_frame(c, 0xA, f + hlen, (size_t)plen);
        } else if (opcode == 0x8) {
            zn_info("[webrtc-signal] ws close");
            zn_tcp_client_close(c->tcp);
            c->ws_open = 0;
            sig_schedule_retry(c);
            return;
        }
        off += total;
    }
    if (off > 0) {
        memmove(c->rx, c->rx + off, c->rx_len - off);
        c->rx_len -= off;
    }
}

/*
 * 收包回调，分两个阶段：
 *   1. 握手阶段：累积到 "\r\n\r\n"，校验 101 响应；握手成功那一刻，服务端
 *      可能已经在同一段数据里捎带了首个 WS 帧，所以要把头部之后的剩余字节
 *      （rest）搬进 rx，交给 sig_parse_frames，否则会丢消息。
 *   2. 握手完成后：直接追加到 rx 并解析。
 * 任一缓冲区溢出都清零重来，宁可丢当前批也不能越界写。
 */
static void sig_on_recv(zn_tcp_client *client, const void *data, size_t len, void *user)
{
    sig_ctx *c = (sig_ctx *)user;
    (void)client;

    if (!c || c->stop || !data || len == 0) {
        return;
    }
    if (!c->hs_done) {
        if (c->hs_len + len > sizeof(c->hs) - 1) {
            c->hs_len = 0;
            return;
        }
        memcpy(c->hs + c->hs_len, data, len);
        c->hs_len += len;
        c->hs[c->hs_len] = '\0';
        {
            char *end = strstr(c->hs, "\r\n\r\n");
            if (!end) {
                return;
            }
            if (strstr(c->hs, " 101 ") == NULL) {
                zn_warn("[webrtc-signal] ws handshake rejected");
                zn_tcp_client_close(c->tcp);
                sig_schedule_retry(c);
                return;
            }
            c->hs_done = 1;
            c->ws_open = 1;
            zn_info("[webrtc-signal] ws open");
            {
                size_t used = (size_t)(end + 4 - c->hs);
                size_t rest = c->hs_len - used;
                if (rest > 0 && rest < sizeof(c->rx)) {
                    memcpy(c->rx, c->hs + used, rest);
                    c->rx_len = rest;
                }
                c->hs_len = 0;
            }
            sig_send_join(c);
        }
    } else {
        if (c->rx_len + len > sizeof(c->rx)) {
            c->rx_len = 0;
            zn_warn("[webrtc-signal] rx overflow");
            return;
        }
        memcpy(c->rx + c->rx_len, data, len);
        c->rx_len += len;
    }
    if (c->ws_open) {
        sig_parse_frames(c);
    }
}

/*
 * TCP 连接建立：发起 WebSocket 升级握手。
 *
 * Sec-WebSocket-Key 按 RFC 需要是 16 字节随机数的 base64；这里用单调时钟
 * 与对象地址混入一个简单 LCG 生成，只为满足握手格式，不用于安全用途。
 * token 作为查询参数拼到 path 上（已有 '?' 则用 '&'）。握手前清空所有
 * 解析状态，确保重连复用同一 ctx 时不残留上一次的残留字节/标志。
 */
static void sig_on_connect(zn_tcp_client *client, void *user)
{
    sig_ctx *c = (sig_ctx *)user;
    char key_raw[16];
    char key[32];
    char req[768];
    char req_path[384];
    size_t i;
    uint64_t seed;
    char host_hdr[160];

    (void)client;
    if (!c || c->stop) {
        return;
    }
    seed = zn_monotonic_ms() ^ (uintptr_t)c;
    for (i = 0; i < sizeof(key_raw); ++i) {
        key_raw[i] = (char)((seed >> ((i % 8) * 8)) ^ (i * 37 + 11));
        seed = seed * 6364136223846793005ULL + 1;
    }
    (void)zms_base64_encode((const uint8_t *)key_raw, sizeof(key_raw), key, sizeof(key), NULL);
    if (c->port == 80) {
        snprintf(host_hdr, sizeof(host_hdr), "%s", c->host);
    } else {
        snprintf(host_hdr, sizeof(host_hdr), "%s:%u", c->host, (unsigned)c->port);
    }
    if (c->token[0]) {
        snprintf(req_path, sizeof(req_path), "%s%ctoken=%s", c->path,
                 strchr(c->path, '?') ? '&' : '?', c->token);
    } else {
        snprintf(req_path, sizeof(req_path), "%s", c->path);
    }
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n",
             req_path, host_hdr, key);
    c->hs_len = 0;
    c->hs_done = 0;
    c->ws_open = 0;
    c->rx_len = 0;
    if (sig_send_raw(c, req, strlen(req)) != 0) {
        zn_tcp_client_close(c->tcp);
        sig_schedule_retry(c);
        return;
    }
    zn_info("[webrtc-signal] ws upgrading %s:%u%s", c->host, (unsigned)c->port, c->path);
}

/* 连接错误/断开：销毁所有 viewer 的 WebRTC 会话再重连。
 * 会话依赖这条信令链路做候选交换，链路断了它们已无意义，留着只会占资源；
 * 重连成功后由新的 offer 重建。 */
static void sig_on_error(zn_tcp_client *client, void *user)
{
    sig_ctx *c = (sig_ctx *)user;
    (void)client;

    if (!c || c->stop) {
        return;
    }
    zn_warn("[webrtc-signal] ws error/closed, reconnect in %d ms", SIG_RETRY_MS);
    sig_destroy_all(c);
    c->ws_open = 0;
    c->hs_done = 0;
    c->hs_len = 0;
    c->rx_len = 0;
    zn_tcp_client_close(c->tcp);
    sig_schedule_retry(c);
}

static const zn_tcp_client_ops_t k_sig_ops = {
    sig_on_connect,
    sig_on_recv,
    sig_on_error,
};

/* 解析 ws://host[:port]/path[?token=...]。token 既可能来自查询串也可能来自
 * 启动参数（由调用方决定优先级）。path 不含查询串；查询串里的 token=xxx 会被
 * 单独提取——这样后续拼接 token 时不会重复/污染 path。 */
static int sig_parse_url(const char *url, char *host, size_t host_cap, uint16_t *port, char *path,
                         size_t path_cap, char *token, size_t token_cap)
{
    const char *p;
    const char *slash;
    const char *colon;
    const char *q;
    size_t n;

    if (!url || strncmp(url, "ws://", 5) != 0) {
        return -1;
    }
    p = url + 5;
    slash = strchr(p, '/');
    if (!slash) {
        return -1;
    }
    colon = memchr(p, ':', (size_t)(slash - p));
    n = colon ? (size_t)(colon - p) : (size_t)(slash - p);
    if (n == 0 || n >= host_cap) {
        return -1;
    }
    memcpy(host, p, n);
    host[n] = '\0';
    *port = colon ? (uint16_t)strtoul(colon + 1, NULL, 10) : 80;
    if (*port == 0) {
        *port = 80;
    }
    q = strchr(slash, '?');
    n = q ? (size_t)(q - slash) : strlen(slash);
    if (n >= path_cap) {
        return -1;
    }
    memcpy(path, slash, n);
    path[n] = '\0';
    token[0] = '\0';
    if (q) {
        const char *t = strstr(q, "token=");
        if (t) {
            t += 6;
            n = strcspn(t, "&");
            if (n >= token_cap) {
                n = token_cap - 1;
            }
            memcpy(token, t, n);
            token[n] = '\0';
        }
    }
    return 0;
}

int zms_webrtc_signaling_start(const char *url, const char *room, const char *token,
                               const char *app, const char *stream)
{
    sig_ctx *c;
    zn_poller_pool_opts_t popts;
    zn_tcp_client_opts_t copts;

    if (!url || !url[0] || g_sig) {
        return -1;
    }
    c = (sig_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    if (sig_parse_url(url, c->host, sizeof(c->host), &c->port, c->path, sizeof(c->path), c->token,
                      sizeof(c->token)) != 0) {
        free(c);
        return -1;
    }
    snprintf(c->room, sizeof(c->room), "%s", room && room[0] ? room : "door-1");
    if (token && token[0] && !c->token[0]) {
        snprintf(c->token, sizeof(c->token), "%s", token);
    }
    snprintf(c->app, sizeof(c->app), "%s", app && app[0] ? app : "live");
    snprintf(c->stream, sizeof(c->stream), "%s", stream && stream[0] ? stream : "cam1");
    snprintf(c->client_id, sizeof(c->client_id), "zms-%llx",
             (unsigned long long)(zn_monotonic_ms() & 0xffffff));

    /* 信令客户端跑在自己的单线程 poller 池里（size=1）：WebRTC 会话的
     * ICE/TCP 回调都串行落在这个线程上，无需在 ctx 内部再加锁。 */
    memset(&popts, 0, sizeof(popts));
    popts.size = 1;
    popts.prefer_current_thread = 0;
    popts.thread_priority = ZB_THREAD_PRIO_NORMAL;
    c->pool = zn_poller_pool_create(&popts);
    if (!c->pool || zn_poller_pool_start(c->pool) != ZN_OK) {
        if (c->pool) {
            zn_poller_pool_destroy(c->pool);
        }
        free(c);
        return -1;
    }
    c->poller = zn_poller_pool_get_first(c->pool);
    if (!c->poller) {
        zn_poller_pool_stop(c->pool);
        zn_poller_pool_destroy(c->pool);
        free(c);
        return -1;
    }
    memset(&copts, 0, sizeof(copts));
    copts.poller = c->poller;
    copts.ops = &k_sig_ops;
    copts.user = c;
    c->tcp = zn_tcp_client_create(&copts);
    if (!c->tcp) {
        zn_poller_pool_stop(c->pool);
        zn_poller_pool_destroy(c->pool);
        free(c);
        return -1;
    }
    g_sig = c;
    if (zn_tcp_client_connect(c->tcp, c->host, c->port) != ZN_OK) {
        sig_schedule_retry(c);
    }
    zn_info("[webrtc-signal] client started %s room=%s app=%s stream=%s", url, c->room, c->app,
             c->stream);
    return 0;
}

void zms_webrtc_signaling_stop(void)
{
    sig_ctx *c = g_sig;

    if (!c) {
        return;
    }
    g_sig = NULL;
    c->stop = 1;
    if (c->tcp) {
        zn_tcp_client_close(c->tcp);
        zn_tcp_client_destroy(c->tcp);
        c->tcp = NULL;
    }
    sig_destroy_all(c);
    if (c->pool) {
        zn_poller_pool_stop(c->pool);
        zn_poller_pool_destroy(c->pool);
        c->pool = NULL;
    }
    free(c);
    zn_info("[webrtc-signal] client stopped");
}
