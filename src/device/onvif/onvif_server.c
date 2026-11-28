/**
 * @file onvif_server.c
 * @brief ONVIF 设备端：WS-Discovery 应答 + Device/Media1 SOAP 服务（HTTP 路由）。
 *
 * 设计取舍：
 *   - 不做持久化：profile/RTSP URI 全部由 `zcs_media_source_foreach` 现算，
 *     设备端永远反映“当前活着的流”，避免注册表与 ONVIF 视图不一致的同步问题。
 *   - 响应走 sb_t 单缓冲累加 + 一次性发送：ONVIF 客户端（NVR/解码器）对
 *     残缺 XML 零容忍，任何截断都必须升级成 SOAP Fault，而不是发出半截。
 *   - WS-Discovery 不占专用线程：组播 socket 挂进 poller（见 wsd_on_readable），
 *     与 HTTP 处理天然同线程，省掉对 g_srv 全局状态的锁。
 *   - 全局状态 g_srv 只在启动/停止时改写，运行期只读；PullPoint 的投递游标
 *     g_event_cursor_ms 是单订阅假设（本设备端只支持一个拉取点）。
 */
#include "zcs/device/onvif/onvif_server.h"
#include "zcs/device/core/device_command.h"
#include "zcs/device/core/device_core.h"
#include "zcs/device/core/device_event.h"
#include "zcs/device/core/device_ptz.h"
#include "device/onvif/onvif_internal.h"
#include "zcs/core/stream/hub.h"
#include "zcs/core/stream_track.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/protocol/http/route.h"
#include "zcs/protocol/http/session_api.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include "zn/poller/poller.h"
#include "zn/util/timer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET onvif_sock_t;
#define ONVIF_SOCK_INVALID INVALID_SOCKET
#define onvif_sock_close closesocket
typedef int onvif_socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int onvif_sock_t;
#define ONVIF_SOCK_INVALID (-1)
#define onvif_sock_close close
typedef socklen_t onvif_socklen_t;
#endif

#define ONVIF_WSD_GROUP "239.255.255.250"
#define ONVIF_WSD_PORT 3702

static struct {
    int enabled;
    char host[128];
    unsigned http_port;
    unsigned rtsp_port;
    char name[64];
    char uuid[80];
    int discovery;
} g_srv;

static int g_routes_registered;
/** 单个 PullPoint 订阅的投递游标（创建订阅时从当前时间开始）。 */
static uint64_t g_event_cursor_ms;

/* ── 小工具 ─────────────────────────────────────────────────────────────── */

typedef struct {
    char *b;
    size_t cap;
    size_t len;
    /** 1 = 期间发生过截断（调用方必须放弃本次响应，不能发送残缺 XML） */
    int overflow;
} sb_t;

static void sb_init(sb_t *w, char *buf, size_t cap)
{
    w->b = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
    if (cap) {
        buf[0] = '\0';
    }
}

static void sb_puts(sb_t *w, const char *s)
{
    while (s && *s) {
        if (w->len + 1 < w->cap) {
            w->b[w->len++] = *s;
            w->b[w->len] = '\0';
        } else {
            w->overflow = 1;
            return;
        }
        ++s;
    }
}

/*
 * 追加格式化文本。overflow 一旦置位就永久保持，后续写入全部丢弃：
 * 调用方只需在最后检查一次 overflow，不必逐次判断，且保证“要么完整、
 * 要么整体判失败”，杜绝发送被截断的 XML。
 */
static void sb_fmt(sb_t *w, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (w->len + 1 >= w->cap) {
        w->overflow = 1;
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(w->b + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        w->overflow = 1;
        return;
    }
    if ((size_t)n >= w->cap - w->len) {
        w->len = w->cap - 1;
        w->overflow = 1;
        return;
    }
    w->len += (size_t)n;
}

/* XML 文本转义：ANY 用户可控字符串（流名、通道名）写进 SOAP 前必须过一遍，
 * 否则名字里一个 '&' 就会让对端把整包解析失败。 */
static void sb_esc(sb_t *w, const char *s)
{
    while (s && *s) {
        switch (*s) {
        case '&':
            sb_puts(w, "&amp;");
            break;
        case '<':
            sb_puts(w, "&lt;");
            break;
        case '>':
            sb_puts(w, "&gt;");
            break;
        case '"':
            sb_puts(w, "&quot;");
            break;
        default:
            if (w->len + 1 < w->cap) {
                w->b[w->len++] = *s;
                w->b[w->len] = '\0';
            } else {
                w->overflow = 1;
                return;
            }
            break;
        }
        ++s;
    }
}

static size_t env_open(sb_t *w)
{
    sb_puts(w,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
            " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
            " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
            " xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\""
            " xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\""
            " xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\""
            " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""
            " xmlns:tns1=\"http://www.onvif.org/ver10/topics\""
            " xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
            "<s:Body>");
    return w->len;
}

static size_t env_close(sb_t *w)
{
    sb_puts(w, "</s:Body></s:Envelope>");
    return w->len;
}

static void device_addr(char *out, size_t cap, const char *service)
{
    snprintf(out, cap, "http://%s:%u/onvif/%s", g_srv.host[0] ? g_srv.host : "127.0.0.1",
             g_srv.http_port, service);
}

static const char *encoding_name(zcs_codec_id codec)
{
    switch (codec) {
    case ZCS_CODEC_H264:
        return "H264";
    case ZCS_CODEC_H265:
        return "H265";
    default:
        return NULL;
    }
}

static const char *audio_encoding_name(zcs_codec_id codec)
{
    switch (codec) {
    case ZCS_CODEC_AAC:
        return "AAC";
    case ZCS_CODEC_G711A:
        return "G711";
    case ZCS_CODEC_G711U:
        return "G711";
    default:
        return NULL;
    }
}

/*
 * profile token 只能是 [A-Za-z0-9_-]，其余一律替换成 '_'。
 * token 会出现在 XML 属性里并被客户端回传（GetStreamUri 的 ProfileToken），
 * 因此必须裁掉 '/'、'.'、空格等非标识符字符，既避免 XML 注入，也保证
 * app/stream 拼接出的 key 不会与路径分隔符混淆。
 */
static void sanitize_token(const char *in, char *out, size_t cap)
{
    size_t i;
    for (i = 0; in && in[i] && i + 1 < cap; ++i) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '-') {
            out[i] = c;
        } else {
            out[i] = '_';
        }
    }
    out[i] = '\0';
}

/* 拼接 profile token。默认 vhost 故意不进 token：绝大多数流都挂在默认 vhost 上，
 * 省掉前缀可让 token 更短、更贴近常见 NVR 的 app_stream 命名习惯；非默认 vhost
 * 才加前缀以区分同名流。 */
static void make_key_token(const char *vhost, const char *app, const char *stream, char *out,
                           size_t cap)
{
    char raw[ZCS_VHOST_MAX + ZCS_APP_MAX + ZCS_STREAM_MAX + 8];
    if (vhost && vhost[0] && strcmp(vhost, ZCS_DEFAULT_VHOST) != 0) {
        snprintf(raw, sizeof(raw), "%s_%s_%s", vhost, app, stream);
    } else {
        snprintf(raw, sizeof(raw), "%s_%s", app, stream);
    }
    sanitize_token(raw, out, cap);
}

static void make_profile_token(const zcs_media_source *s, char *out, size_t cap)
{
    make_key_token(s->vhost, s->app, s->stream, out, cap);
}

/*
 * 只有“直播中且已解复用到 GOP 队列”的源才算可播放。
 * 特意排除 vod_buffer：ONVIF 是实时监控协议，点播源不该出现在 profile 列表里；
 * 没有 gop_queue 的源（纯转发/尚未起播）也没有可取的编码参数，同样跳过。
 */
static int source_playable(const zcs_media_source *s)
{
    if (!s || !s->gop_queue || s->vod_buffer) {
        return 0;
    }
    return s->publishing || ((s->has_video || s->has_audio) && s->video.ready);
}

static int ptz_advertised(void);

/* ── 服务响应 ───────────────────────────────────────────────────────────── */

typedef struct {
    sb_t w;
    int count;
} profiles_ctx;

static int profiles_visit(zcs_media_source *s, void *user)
{
    profiles_ctx *ctx = (profiles_ctx *)user;
    sb_t *w = &ctx->w;
    char token[128];
    const char *enc;
    char name[160];
    zcs_device_channel_info ch;

    if (!source_playable(s)) {
        return 0;
    }
    enc = encoding_name(s->video.codec);
    if (!enc) {
        return 0;
    }
    make_profile_token(s, token, sizeof(token));
    /* 通道名来自设备核心；取不到就退化用 app/stream，保证 Name 永不为空——
     * 部分 NVR 会拒绝 Name 为空的 Profile。 */
    if (zcs_device_core_get_key(s->vhost, s->app, s->stream, &ch) == 0 && ch.name[0]) {
        snprintf(name, sizeof(name), "%s", ch.name);
    } else {
        snprintf(name, sizeof(name), "%s/%s", s->app, s->stream);
    }
    ++ctx->count;

    sb_fmt(w, "<trt:Profiles token=\"");
    sb_esc(w, token);
    sb_puts(w, "\" fixed=\"true\">");
    sb_puts(w, "<tt:Name>");
    sb_esc(w, name);
    sb_puts(w, "</tt:Name>");

    sb_puts(w, "<tt:VideoEncoderConfiguration token=\"");
    sb_esc(w, token);
    sb_puts(w, "_ve\">");
    sb_puts(w, "<tt:Name>VideoEncoder</tt:Name><tt:UseCount>1</tt:UseCount>");
    sb_fmt(w, "<tt:Encoding>%s</tt:Encoding>", enc);
    sb_fmt(w,
           "<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>",
           s->video.width > 0 ? s->video.width : 1920,
           s->video.height > 0 ? s->video.height : 1080);
    sb_puts(w, "<tt:Quality>5</tt:Quality>");
    sb_fmt(w,
           "<tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit>"
           "<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>4096</tt:BitrateLimit>"
           "</tt:RateControl>",
           s->video.fps > 0.5f ? (int)(s->video.fps + 0.5f) : 25);
    if (strcmp(enc, "H265") == 0) {
        sb_puts(w, "<tt:H265><tt:GovLength>50</tt:GovLength></tt:H265>");
    } else {
        sb_puts(w, "<tt:H264><tt:GovLength>50</tt:GovLength><tt:H264Profile>Main</tt:H264Profile>"
                   "</tt:H264>");
    }
    sb_puts(w, "</tt:VideoEncoderConfiguration>");

    {
        const char *aenc = s->has_audio ? audio_encoding_name(s->audio.codec) : NULL;
        if (aenc) {
            sb_puts(w, "<tt:AudioEncoderConfiguration token=\"");
            sb_esc(w, token);
            sb_puts(w, "_ae\">");
            sb_puts(w, "<tt:Name>AudioEncoder</tt:Name><tt:UseCount>1</tt:UseCount>");
            sb_fmt(w, "<tt:Encoding>%s</tt:Encoding>", aenc);
            sb_fmt(w,
                   "<tt:SampleRate>%d</tt:SampleRate><tt:Bitrate>64</tt:Bitrate>"
                   "<tt:Channels>%d</tt:Channels>",
                   s->audio.sample_rate > 0 ? s->audio.sample_rate : 16000,
                   s->audio.channels > 0 ? s->audio.channels : 1);
            sb_puts(w, "</tt:AudioEncoderConfiguration>");
        }
    }
    sb_puts(w, "</trt:Profiles>");
    return 0;
}

typedef struct {
    const char *token;
    char vhost[ZCS_VHOST_MAX];
    char app[ZCS_APP_MAX];
    char stream[ZCS_STREAM_MAX];
    int found;
} uri_ctx;

static int uri_visit(zcs_media_source *s, void *user)
{
    uri_ctx *ctx = (uri_ctx *)user;
    char token[128];

    if (!source_playable(s)) {
        return 0;
    }
    make_profile_token(s, token, sizeof(token));
    if (strcmp(token, ctx->token) != 0) {
        return 0;
    }
    snprintf(ctx->vhost, sizeof(ctx->vhost), "%s", s->vhost);
    snprintf(ctx->app, sizeof(ctx->app), "%s", s->app);
    snprintf(ctx->stream, sizeof(ctx->stream), "%s", s->stream);
    ctx->found = 1;
    return 1;
}

static void build_rtsp_uri(const char *vhost, const char *app, const char *stream, char *out,
                           size_t cap)
{
    const char *host = g_srv.host[0] ? g_srv.host : "127.0.0.1";
    if (vhost && vhost[0] && strcmp(vhost, ZCS_DEFAULT_VHOST) != 0) {
        /* 非默认 vhost：ZCS 从 URL 主机名解析 vhost */
        host = vhost;
    }
    snprintf(out, cap, "rtsp://%s:%u/%s/%s", host, g_srv.rtsp_port, app, stream);
}

/* ── SOAP 处理 ──────────────────────────────────────────────────────────── */

static void respond_fault(zcs_http_session *hs, const char *subcode, const char *reason);

/* 统一的成功出口：补上信封闭合，检查截断后再发送。
 * 截断时改发 Fault——宁可让客户端收到一个明确的错误，也不能让它拿到
 * 非法的半截 XML（很多 NVR 收到坏包会直接断开并反复重连）。 */
static void respond_soap(zcs_http_session *hs, sb_t *w)
{
    env_close(w);
    if (w->overflow) {
        zn_error("onvif: SOAP response overflow (cap=%u), sending fault", (unsigned)w->cap);
        respond_fault(hs, "OutOfMemory", "response too large");
        return;
    }
    zcs_http_response_send_bytes(hs, 200, "application/soap+xml; charset=utf-8", w->b, w->len);
}

static void respond_fault(zcs_http_session *hs, const char *subcode, const char *reason)
{
    char buf[1024];
    sb_t w;
    sb_init(&w, buf, sizeof(buf));
    sb_puts(&w,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
            " xmlns:ter=\"http://www.onvif.org/ver10/error\"><s:Body><s:Fault>"
            "<s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>ter:");
    sb_esc(&w, subcode);
    sb_puts(&w, "</s:Value></s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">");
    sb_esc(&w, reason);
    sb_puts(&w, "</s:Text></s:Reason></s:Fault></s:Body></s:Envelope>");
    zcs_http_response_send_bytes(hs, 400, "application/soap+xml; charset=utf-8", w.b, w.len);
}

/*
 * ONVIF 客户端几乎总把 GetSystemDateAndTime 作为握手第一步：部分实现会据此
 * 校验与本机时钟偏差（偏太大就拒绝后续鉴权）。这里统一报 UTC（不论本地时区），
 * 并把 DateTimeType 标成 NTP——虽然设备本身未跑 NTP，但声明为 NTP 能让客户端
 * 跳过“需要先设置时间的初始化流程”。 */
static void handle_get_system_date_time(zcs_http_session *hs)
{
    char buf[2048];
    sb_t w;
    time_t t = time(NULL);
    struct tm tmv;

#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime>");
    sb_puts(&w, "<tt:DateTimeType>NTP</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings>");
    sb_fmt(&w,
           "<tt:UTCDateTime><tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute>"
           "<tt:Second>%d</tt:Second></tt:Time>"
           "<tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month><tt:Day>%d</tt:Day></tt:Date>"
           "</tt:UTCDateTime>",
           tmv.tm_hour, tmv.tm_min, tmv.tm_sec, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    sb_puts(&w, "</tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>");
    respond_soap(hs, &w);
}

static void handle_get_device_information(zcs_http_session *hs)
{
    char buf[2048];
    sb_t w;
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tds:GetDeviceInformationResponse>");
    sb_puts(&w, "<tds:Manufacturer>ZCS</tds:Manufacturer>");
    sb_puts(&w, "<tds:Model>ZCS Media Server</tds:Model>");
    sb_fmt(&w, "<tds:FirmwareVersion>%s</tds:FirmwareVersion>", ZCS_VERSION_STRING);
    sb_puts(&w, "<tds:SerialNumber>");
    sb_esc(&w, g_srv.uuid);
    sb_puts(&w, "</tds:SerialNumber><tds:HardwareId>ZCS</tds:HardwareId>");
    sb_puts(&w, "</tds:GetDeviceInformationResponse>");
    respond_soap(hs, &w);
}

/*
 * 能力宣告统一按 ptz_advertised() 门控：PTZ 节点/服务只在实际存在后端或
 * 设备核心声明了 PTZ 能力时才出现。漏掉这一层会让没有云台的部署也被客户端
 * 认为支持 PTZ，进而在界面上弹出无法响应的云台控件。
 */
static void handle_get_capabilities(zcs_http_session *hs)
{
    char buf[4096];
    char dev[256];
    char media[256];
    char ptz[256];
    char events[256];
    sb_t w;

    device_addr(dev, sizeof(dev), "device_service");
    device_addr(media, sizeof(media), "media_service");
    device_addr(ptz, sizeof(ptz), "ptz_service");
    device_addr(events, sizeof(events), "events_service");
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tds:GetCapabilitiesResponse><tds:Capabilities>");
    sb_puts(&w, "<tt:Device><tt:XAddr>");
    sb_esc(&w, dev);
    sb_puts(&w, "</tt:XAddr></tt:Device>");
    sb_puts(&w, "<tt:Media><tt:XAddr>");
    sb_esc(&w, media);
    sb_puts(&w,
            "</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
            "<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
            "</tt:StreamingCapabilities></tt:Media>");
    if (ptz_advertised()) {
        sb_puts(&w, "<tt:PTZ><tt:XAddr>");
        sb_esc(&w, ptz);
        sb_puts(&w, "</tt:XAddr></tt:PTZ>");
    }
    sb_puts(&w, "<tt:Events><tt:XAddr>");
    sb_esc(&w, events);
    sb_puts(&w,
            "</tt:XAddr><tt:WSSubscriptionPolicySupport>true</tt:WSSubscriptionPolicySupport>"
            "<tt:WSPullPointSupport>true</tt:WSPullPointSupport>"
            "<tt:WSPausableSubscriptionManagerInterfaceSupport>false"
            "</tt:WSPausableSubscriptionManagerInterfaceSupport></tt:Events>");
    sb_puts(&w, "</tds:Capabilities></tds:GetCapabilitiesResponse>");
    respond_soap(hs, &w);
}

static void handle_get_services(zcs_http_session *hs)
{
    char buf[4096];
    char dev[256];
    char media[256];
    char ptz[256];
    char events[256];
    sb_t w;

    device_addr(dev, sizeof(dev), "device_service");
    device_addr(media, sizeof(media), "media_service");
    device_addr(ptz, sizeof(ptz), "ptz_service");
    device_addr(events, sizeof(events), "events_service");
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tds:GetServicesResponse>");
    sb_puts(&w, "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/device/wsdl"
                "</tds:Namespace><tds:XAddr>");
    sb_esc(&w, dev);
    sb_puts(&w, "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor>"
                "</tds:Version></tds:Service>");
    sb_puts(&w, "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/media/wsdl"
                "</tds:Namespace><tds:XAddr>");
    sb_esc(&w, media);
    sb_puts(&w, "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor>"
                "</tds:Version></tds:Service>");
    if (ptz_advertised()) {
        sb_puts(&w, "<tds:Service><tds:Namespace>http://www.onvif.org/ver20/ptz/wsdl"
                    "</tds:Namespace><tds:XAddr>");
        sb_esc(&w, ptz);
        sb_puts(&w, "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor>"
                    "</tds:Version></tds:Service>");
    }
    sb_puts(&w, "<tds:Service><tds:Namespace>http://www.onvif.org/ver10/events/wsdl"
                "</tds:Namespace><tds:XAddr>");
    sb_esc(&w, events);
    sb_puts(&w, "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>6</tt:Minor>"
                "</tds:Version></tds:Service>");
    sb_puts(&w, "</tds:GetServicesResponse>");
    respond_soap(hs, &w);
}

static void handle_get_network_interfaces(zcs_http_session *hs)
{
    char buf[2048];
    sb_t w;
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"eth0\">");
    sb_puts(&w, "<tt:Enabled>true</tt:Enabled><tt:Info><tt:Name>eth0</tt:Name>"
                "<tt:HwAddress>00:00:00:00:00:00</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info>");
    sb_puts(&w, "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:Manual><tt:Address>");
    sb_esc(&w, g_srv.host[0] ? g_srv.host : "127.0.0.1");
    sb_puts(&w, "</tt:Address><tt:PrefixLength>24</tt:PrefixLength></tt:Manual>"
                "<tt:DHCP>false</tt:DHCP></tt:Config></tt:IPv4>");
    sb_puts(&w, "</tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>");
    respond_soap(hs, &w);
}

static void handle_get_profiles(zcs_http_session *hs)
{
    /*
     * 上限按"每路 profile 约 1~1.5KB × 并发直播源"估算：64KB 可容纳约 40 路。
     * 用堆分配避免大栈帧；超限由 sb_t.overflow 显式报错（不发残缺 XML）。
     */
    char *buf = (char *)malloc(ZCS_ONVIF_BODY_MAX);
    profiles_ctx ctx;

    if (!buf) {
        respond_fault(hs, "OutOfMemory", "GetProfiles buffer");
        return;
    }
    sb_init(&ctx.w, buf, ZCS_ONVIF_BODY_MAX);
    env_open(&ctx.w);
    sb_puts(&ctx.w, "<trt:GetProfilesResponse>");
    ctx.count = 0;
    zcs_device_core_sync_live();
    zcs_media_source_foreach(profiles_visit, &ctx, NULL);
    sb_puts(&ctx.w, "</trt:GetProfilesResponse>");
    respond_soap(hs, &ctx.w);
    free(buf);
}

static void handle_get_video_sources(zcs_http_session *hs)
{
    char buf[2048];
    sb_t w;
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"vs0\">");
    sb_puts(&w, "<tt:Framerate>25</tt:Framerate><tt:Resolution><tt:Width>1920</tt:Width>"
                "<tt:Height>1080</tt:Height></tt:Resolution>");
    sb_puts(&w, "<tt:Imaging><tt:Focus><tt:AutoFocusMode>MANUAL</tt:AutoFocusMode></tt:Focus>"
                "</tt:Imaging>");
    sb_puts(&w, "</trt:VideoSources></trt:GetVideoSourcesResponse>");
    respond_soap(hs, &w);
}

/*
 * GetStreamUri：把客户端回的 ProfileToken 反查回 (vhost,app,stream)，再拼 RTSP URI。
 * 必须重新遍历源而不是在 profile token 里编码路径，是因为 token 经过 sanitize、
 * 不可逆；而源集合随时变化，只有现查才能保证返回的 URI 指向真实存在的流。
 */
static void handle_get_stream_uri(zcs_http_session *hs, const char *req_body)
{
    char token[128];
    char buf[2048];
    char uri[512];
    sb_t w;
    uri_ctx ctx;

    memset(&ctx, 0, sizeof(ctx));
    if (zcs_xml_text(req_body, "ProfileToken", token, sizeof(token)) != 0) {
        respond_fault(hs, "InvalidArgVal", "missing ProfileToken");
        return;
    }
    ctx.token = token;
    zcs_media_source_foreach(uri_visit, &ctx, NULL);
    if (!ctx.found) {
        respond_fault(hs, "NoProfile", "profile not found");
        return;
    }
    build_rtsp_uri(ctx.vhost, ctx.app, ctx.stream, uri, sizeof(uri));
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>");
    sb_esc(&w, uri);
    sb_puts(&w,
            "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>"
            "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout>"
            "</trt:MediaUri></trt:GetStreamUriResponse>");
    respond_soap(hs, &w);
}

/* ── PTZ / Events 服务（M6） ───────────────────────────────────────────── */

static void utc_iso_after(time_t t, char *out, size_t cap)
{
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static int ptz_advertised(void)
{
    return zcs_device_core_any_cap(ZCS_DEV_CAP_PTZ) || zcs_device_ptz_has_backend();
}

typedef struct {
    const char *token;
    char id[ZCS_DEVICE_ID_MAX];
    int found;
} token_channel;

static int token_channel_visit(const zcs_device_channel_info *ch, void *user)
{
    token_channel *ctx = (token_channel *)user;
    char token[128];

    make_key_token(ch->vhost, ch->app, ch->stream, token, sizeof(token));
    if (strcmp(token, ctx->token) == 0 || strcmp(ch->id, ctx->token) == 0 ||
        (ch->gb_id[0] && strcmp(ch->gb_id, ctx->token) == 0)) {
        snprintf(ctx->id, sizeof(ctx->id), "%s", ch->id);
        ctx->found = 1;
        return 1;
    }
    return 0;
}

/*
 * 把客户端给的 token 解析成内部 channel id。token 可能是三种形态之一：
 * 合成的 profile token、设备核心的 channel id、或国标 gb_id——不同客户端
 * 回传的习惯不同，全部接受能提高兼容性（见 token_channel_visit）。
 */
static int resolve_profile_channel(const char *token, char *out, size_t cap)
{
    token_channel ctx;

    if (!token || !token[0]) {
        return -1;
    }
    memset(&ctx, 0, sizeof(ctx));
    ctx.token = token;
    zcs_device_core_sync_live();
    zcs_device_core_foreach(0, 0, token_channel_visit, &ctx);
    if (!ctx.found) {
        return -1;
    }
    snprintf(out, cap, "%s", ctx.id);
    return 0;
}

static float clamp_unit(float v)
{
    if (v > 1.0f) {
        return 1.0f;
    }
    if (v < -1.0f) {
        return -1.0f;
    }
    return v;
}

/*
 * 从 ContinuousMove 请求体解析速度分量。
 * ONVIF 的 VelocityGenericSpace 约定三个分量都落在 [-1,1]；但客户端发的值不受
 * 这个约束（存在发 ±100% 的），所以逐个 clamp，绝不能把越界值直接透传给 PTZ
 * 后端（可能触发 backend 的断言或超大幅云台动作）。
 */
static void ptz_parse_velocity(const char *body, zcs_device_ptz_cmd *cmd)
{
    char val[32];

    if (zcs_xml_attr(body, "PanTilt", "x", val, sizeof(val)) == 0) {
        cmd->pan = clamp_unit((float)atof(val));
    }
    if (zcs_xml_attr(body, "PanTilt", "y", val, sizeof(val)) == 0) {
        cmd->tilt = clamp_unit((float)atof(val));
    }
    if (zcs_xml_attr(body, "Zoom", "x", val, sizeof(val)) == 0) {
        cmd->zoom = clamp_unit((float)atof(val));
    }
}

static void ptz_spaces(sb_t *w, const char *wrapper)
{
    sb_fmt(w, "<tt:%s>", wrapper);
    sb_puts(w,
            "<tt:ContinuousPanTiltVelocitySpace>"
            "<tt:URI>http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace</tt:URI>"
            "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
            "<tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>"
            "</tt:ContinuousPanTiltVelocitySpace>"
            "<tt:ContinuousZoomVelocitySpace>"
            "<tt:URI>http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace</tt:URI>"
            "<tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:XRange>"
            "</tt:ContinuousZoomVelocitySpace>");
    sb_fmt(w, "</tt:%s>", wrapper);
}

static void handle_ptz_get_nodes(zcs_http_session *hs)
{
    char buf[2048];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w,
            "<tptz:GetNodesResponse><tptz:PTZNode token=\"ptz0\" FixedHomePosition=\"false\">"
            "<tt:Name>PTZNode</tt:Name>");
    ptz_spaces(&w, "SupportedPTZSpaces");
    sb_puts(&w,
            "<tt:MaximumNumberOfPresets>0</tt:MaximumNumberOfPresets>"
            "<tt:HomeSupported>false</tt:HomeSupported>"
            "</tptz:PTZNode></tptz:GetNodesResponse>");
    respond_soap(hs, &w);
}

static void handle_ptz_get_configurations(zcs_http_session *hs)
{
    char buf[2048];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w,
            "<tptz:GetConfigurationsResponse><tptz:PTZConfiguration token=\"ptz0\">"
            "<tt:Name>PTZConfig</tt:Name><tt:UseCount>1</tt:UseCount>"
            "<tt:NodeToken>ptz0</tt:NodeToken>");
    sb_puts(&w,
            "<tt:DefaultContinuousPanTiltVelocitySpace>"
            "http://www.onvif.org/ver10/tptz/PanTiltSpaces/VelocityGenericSpace"
            "</tt:DefaultContinuousPanTiltVelocitySpace>"
            "<tt:DefaultContinuousZoomVelocitySpace>"
            "http://www.onvif.org/ver10/tptz/ZoomSpaces/VelocityGenericSpace"
            "</tt:DefaultContinuousZoomVelocitySpace>");
    sb_puts(&w, "</tptz:PTZConfiguration></tptz:GetConfigurationsResponse>");
    respond_soap(hs, &w);
}

static void handle_ptz_configuration_options(zcs_http_session *hs)
{
    char buf[2048];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w,
            "<tptz:GetConfigurationOptionsResponse><tptz:PTZConfigurationOptions>");
    ptz_spaces(&w, "Spaces");
    sb_puts(&w,
            "<tt:PTZTimeout><tt:Min>PT1S</tt:Min><tt:Max>PT3600S</tt:Max></tt:PTZTimeout>"
            "</tptz:PTZConfigurationOptions></tptz:GetConfigurationOptionsResponse>");
    respond_soap(hs, &w);
}

static void handle_ptz_continuous_move(zcs_http_session *hs, const char *body)
{
    char token[128];
    char channel[ZCS_DEVICE_ID_MAX];
    char buf[1024];
    zcs_device_ptz_cmd cmd;
    sb_t w;

    memset(&cmd, 0, sizeof(cmd));
    if (zcs_xml_text(body, "ProfileToken", token, sizeof(token)) != 0) {
        respond_fault(hs, "InvalidArgVal", "missing ProfileToken");
        return;
    }
    /* ProfileToken 是 ONVIF 的对外标识，PTZ 后端却要内部 channel_id：
     * 必须经 resolve_profile_channel 做一次映射，不能直接把 token 当 id 用。 */
    if (resolve_profile_channel(token, channel, sizeof(channel)) != 0) {
        respond_fault(hs, "NoProfile", "profile not found");
        return;
    }
    snprintf(cmd.channel_id, sizeof(cmd.channel_id), "%s", channel);
    ptz_parse_velocity(body, &cmd);
    if (zcs_device_ptz_dispatch(&cmd) != 0) {
        respond_fault(hs, "PTZError", "ptz backend failed");
        return;
    }
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tptz:ContinuousMoveResponse/>");
    respond_soap(hs, &w);
}

static void handle_ptz_stop(zcs_http_session *hs, const char *body)
{
    char token[128];
    char channel[ZCS_DEVICE_ID_MAX];
    char buf[1024];
    zcs_device_ptz_cmd cmd;
    sb_t w;

    memset(&cmd, 0, sizeof(cmd));
    if (zcs_xml_text(body, "ProfileToken", token, sizeof(token)) != 0) {
        respond_fault(hs, "InvalidArgVal", "missing ProfileToken");
        return;
    }
    if (resolve_profile_channel(token, channel, sizeof(channel)) != 0) {
        respond_fault(hs, "NoProfile", "profile not found");
        return;
    }
    snprintf(cmd.channel_id, sizeof(cmd.channel_id), "%s", channel);
    cmd.stop = 1;
    if (zcs_device_ptz_dispatch(&cmd) != 0) {
        respond_fault(hs, "PTZError", "ptz backend failed");
        return;
    }
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tptz:StopResponse/>");
    respond_soap(hs, &w);
}

static void handle_ptz_get_status(zcs_http_session *hs, const char *body)
{
    char token[128];
    char channel[ZCS_DEVICE_ID_MAX];
    char now[32];
    char buf[1024];
    sb_t w;

    if (zcs_xml_text(body, "ProfileToken", token, sizeof(token)) != 0 ||
        resolve_profile_channel(token, channel, sizeof(channel)) != 0) {
        respond_fault(hs, "NoProfile", "profile not found");
        return;
    }
    utc_iso_after(time(NULL), now, sizeof(now));
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w,
            "<tptz:GetStatusResponse><tptz:PTZStatus><tt:Position>"
            "<tt:PanTilt x=\"0\" y=\"0\"/><tt:Zoom x=\"0\"/></tt:Position>"
            "<tt:MoveStatus>IDLE</tt:MoveStatus><tt:Error>NoError</tt:Error><tt:UtcTime>");
    sb_esc(&w, now);
    sb_puts(&w, "</tt:UtcTime></tptz:PTZStatus></tptz:GetStatusResponse>");
    respond_soap(hs, &w);
}

/* ── ONVIF Events：PullPoint ───────────────────────────────────────────── */

static void handle_events_properties(zcs_http_session *hs)
{
    char buf[4096];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w,
            "<tev:GetEventPropertiesResponse><wsnt:TopicSet>"
            "<tns1:Device><tns1:Trigger><tns1:Alarm/></tns1:Trigger></tns1:Device>"
            "<tns1:RuleEngine><tns1:CellMotionDetector><tns1:Motion/></tns1:CellMotionDetector>"
            "</tns1:RuleEngine>"
            "</wsnt:TopicSet><tev:FixedTopicSet>true</tev:FixedTopicSet>"
            "<tev:TopicExpressionDialect>"
            "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet"
            "</tev:TopicExpressionDialect>"
            "<tev:MessageContentFilterDialect>"
            "http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter"
            "</tev:MessageContentFilterDialect>"
            "<tev:MessageContentSchemaLocation>"
            "http://www.onvif.org/onvif/ver10/schema/onvif.xsd"
            "</tev:MessageContentSchemaLocation></tev:GetEventPropertiesResponse>");
    respond_soap(hs, &w);
}

static void handle_create_pullpoint(zcs_http_session *hs)
{
    char addr[256];
    char now[32];
    char term[32];
    char buf[1024];
    sb_t w;

    device_addr(addr, sizeof(addr), "events_service");
    /* 单订阅假设：建订阅时把游标重置为“此刻”，之后的 PullMessages 只推尔后
     * 产生的事件。多客户端共享同一游标会互相吞事件，但本设备端只支持一个拉取点，
     * 用单游标换取实现简单。 */
    g_event_cursor_ms = zn_wall_ms();
    utc_iso_after(time(NULL), now, sizeof(now));
    utc_iso_after(time(NULL) + 60, term, sizeof(term));
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>");
    sb_puts(&w, "<wsa:Address>");
    sb_esc(&w, addr);
    sb_puts(&w, "</wsa:Address></tev:SubscriptionReference><wsnt:CurrentTime>");
    sb_esc(&w, now);
    sb_puts(&w, "</wsnt:CurrentTime><wsnt:TerminationTime>");
    sb_esc(&w, term);
    sb_puts(&w, "</wsnt:TerminationTime></tev:CreatePullPointSubscriptionResponse>");
    respond_soap(hs, &w);
}

/* 把内部事件类型映射到 ONVIF 标准 topic 路径。
 * 只覆盖移动侦测与告警两类：客户端通常按 ConcreteSet 里的 topic 精确订阅，
 * 报错一个不在 TopicSet 中的 topic 会让某些客户端丢弃整条通知。 */
static const char *event_topic(int type)
{
    return type == ZCS_DEV_EVENT_MOTION ? "tns1:RuleEngine/CellMotionDetector/Motion"
                                        : "tns1:Device/Trigger/Alarm";
}

static void handle_pull_messages(zcs_http_session *hs)
{
    /* 事件与响应体都放堆上：ONVIF 事件报文可达数 KB，poller 栈只留小缓冲。 */
    zcs_device_event *evs = (zcs_device_event *)calloc(16, sizeof(*evs));
    char *buf = (char *)malloc(ZCS_ONVIF_BODY_MAX);
    size_t n;
    size_t i;
    char now[32];
    char term[32];
    sb_t w;

    if (!evs || !buf) {
        free(evs);
        free(buf);
        zcs_http_response_send_error(hs, 500, "Internal Server Error");
        return;
    }
    n = zcs_device_event_list(evs, 16);
    utc_iso_after(time(NULL), now, sizeof(now));
    utc_iso_after(time(NULL) + 60, term, sizeof(term));
    sb_init(&w, buf, ZCS_ONVIF_BODY_MAX);
    env_open(&w);
    sb_puts(&w, "<tev:PullMessagesResponse><tev:CurrentTime>");
    sb_esc(&w, now);
    sb_puts(&w, "</tev:CurrentTime><tev:TerminationTime>");
    sb_esc(&w, term);
    sb_puts(&w, "</tev:TerminationTime>");
    {
        size_t sent = 0;
        uint64_t next_cursor = g_event_cursor_ms;
        /* 事件缓冲 evs 是“最新在后”的顺序，从尾部往前取（最旧优先），
         * 但一旦越过上次投递游标就停：这保证同一事件不会被重复推送。
         * sent<10 是单次响应的上限，防止一次积压大量事件时把响应撑爆，
         * 也让客户端能分批拉取、及时确认。next_cursor 只在一个事件真正入包后
         * 前移，最后统一提交，避免中途失败导致事件丢失。 */
        for (i = n; i > 0 && sent < 10; --i) {
            const zcs_device_event *ev = &evs[i - 1];
            char ts[32];
            if (ev->time_ms <= g_event_cursor_ms) {
                break;
            }
            utc_iso_after((time_t)(ev->time_ms / 1000u), ts, sizeof(ts));
            sb_puts(&w,
                    "<wsnt:NotificationMessage><wsnt:Topic Dialect=\""
                    "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">");
            sb_puts(&w, event_topic(ev->type));
            sb_puts(&w, "</wsnt:Topic><wsnt:Message><tt:Message UtcTime=\"");
            sb_esc(&w, ts);
            sb_puts(&w, "\" PropertyOperation=\"Changed\"><tt:Source>");
            sb_puts(&w, "<tt:SimpleItem Name=\"Channel\" Value=\"");
            sb_esc(&w, ev->channel_id);
            sb_puts(&w, "\"/></tt:Source><tt:Data>");
            sb_puts(&w, "<tt:SimpleItem Name=\"EventType\" Value=\"");
            sb_esc(&w, zcs_device_event_type_name(ev->type));
            sb_puts(&w, "\"/><tt:SimpleItem Name=\"State\" Value=\"true\"/>");
            sb_puts(&w, "<tt:SimpleItem Name=\"Message\" Value=\"");
            sb_esc(&w, ev->message);
            sb_puts(&w, "\"/></tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>");
            next_cursor = ev->time_ms;
            ++sent;
        }
        if (sent > 0) {
            g_event_cursor_ms = next_cursor;
        }
    }
    sb_puts(&w, "</tev:PullMessagesResponse>");
    respond_soap(hs, &w);
    free(buf);
    free(evs);
}

static void handle_unsubscribe(zcs_http_session *hs)
{
    char buf[512];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<wsnt:UnsubscribeResponse/>");
    respond_soap(hs, &w);
}

static void handle_renew(zcs_http_session *hs)
{
    char term[32];
    char buf[512];
    sb_t w;

    utc_iso_after(time(NULL) + 60, term, sizeof(term));
    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<wsnt:RenewResponse><wsnt:TerminationTime>");
    sb_esc(&w, term);
    sb_puts(&w, "</wsnt:TerminationTime></wsnt:RenewResponse>");
    respond_soap(hs, &w);
}

/* ── HTTP 路由 ──────────────────────────────────────────────────────────── */

/** 只匹配 ONVIF 的固定服务端点，避免与 app 名恰为 "onvif" 的播放入口冲突
 *  （如 /onvif/cam1.flv）。 */
static int path_is_service(const char *path, const char *service)
{
    size_t n = strlen(service);
    if (!path || strncmp(path, service, n) != 0) {
        return 0;
    }
    return path[n] == '\0' || path[n] == '?';
}

static int onvif_route_match(const zcs_http_request *req)
{
    if (!req) {
        return 0;
    }
    return path_is_service(req->path, "/onvif/device_service") ||
           path_is_service(req->path, "/onvif/media_service") ||
           path_is_service(req->path, "/onvif/ptz_service") ||
           path_is_service(req->path, "/onvif/device_io") ||
           path_is_service(req->path, "/onvif/events_service");
}

/* ── 命令表（能力门控 + 统一分发） ─────────────────────────────────────── */

static int cmd_system_time(const zcs_device_cmd_ctx *ctx)
{
    handle_get_system_date_time((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_service_capabilities(const zcs_device_cmd_ctx *ctx)
{
    char buf[512];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tds:GetServiceCapabilitiesResponse/>");
    respond_soap((zcs_http_session *)ctx->session, &w);
    return 0;
}

static int cmd_media_service_capabilities(const zcs_device_cmd_ctx *ctx)
{
    char buf[512];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<trt:GetServiceCapabilitiesResponse/>");
    respond_soap((zcs_http_session *)ctx->session, &w);
    return 0;
}

static int cmd_get_services(const zcs_device_cmd_ctx *ctx)
{
    handle_get_services((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_network_interfaces(const zcs_device_cmd_ctx *ctx)
{
    handle_get_network_interfaces((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_get_capabilities(const zcs_device_cmd_ctx *ctx)
{
    handle_get_capabilities((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_device_information(const zcs_device_cmd_ctx *ctx)
{
    handle_get_device_information((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_get_profiles(const zcs_device_cmd_ctx *ctx)
{
    handle_get_profiles((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_video_sources(const zcs_device_cmd_ctx *ctx)
{
    handle_get_video_sources((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_stream_uri(const zcs_device_cmd_ctx *ctx)
{
    handle_get_stream_uri((zcs_http_session *)ctx->session, (const char *)ctx->message);
    return 0;
}

static int cmd_ptz_service_capabilities(const zcs_device_cmd_ctx *ctx)
{
    char buf[512];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tptz:GetServiceCapabilitiesResponse/>");
    respond_soap((zcs_http_session *)ctx->session, &w);
    return 0;
}

static int cmd_ptz_get_nodes(const zcs_device_cmd_ctx *ctx)
{
    handle_ptz_get_nodes((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_ptz_get_configurations(const zcs_device_cmd_ctx *ctx)
{
    handle_ptz_get_configurations((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_ptz_configuration_options(const zcs_device_cmd_ctx *ctx)
{
    handle_ptz_configuration_options((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_ptz_continuous_move(const zcs_device_cmd_ctx *ctx)
{
    handle_ptz_continuous_move((zcs_http_session *)ctx->session, (const char *)ctx->message);
    return 0;
}

static int cmd_ptz_stop(const zcs_device_cmd_ctx *ctx)
{
    handle_ptz_stop((zcs_http_session *)ctx->session, (const char *)ctx->message);
    return 0;
}

static int cmd_ptz_get_status(const zcs_device_cmd_ctx *ctx)
{
    handle_ptz_get_status((zcs_http_session *)ctx->session, (const char *)ctx->message);
    return 0;
}

static int cmd_events_service_capabilities(const zcs_device_cmd_ctx *ctx)
{
    char buf[512];
    sb_t w;

    sb_init(&w, buf, sizeof(buf));
    env_open(&w);
    sb_puts(&w, "<tev:GetServiceCapabilitiesResponse/>");
    respond_soap((zcs_http_session *)ctx->session, &w);
    return 0;
}

static int cmd_events_properties(const zcs_device_cmd_ctx *ctx)
{
    handle_events_properties((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_create_pullpoint(const zcs_device_cmd_ctx *ctx)
{
    handle_create_pullpoint((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_pull_messages(const zcs_device_cmd_ctx *ctx)
{
    handle_pull_messages((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_unsubscribe(const zcs_device_cmd_ctx *ctx)
{
    handle_unsubscribe((zcs_http_session *)ctx->session);
    return 0;
}

static int cmd_renew(const zcs_device_cmd_ctx *ctx)
{
    handle_renew((zcs_http_session *)ctx->session);
    return 0;
}

static const zcs_device_cmd k_onvif_device_cmds[] = {
    {"GetSystemDateAndTime", "action", 0, cmd_system_time},
    {"GetDeviceInformation", "action", 0, cmd_device_information},
    {"GetServiceCapabilities", "action", 0, cmd_service_capabilities},
    {"GetServices", "action", 0, cmd_get_services},
    {"GetNetworkInterfaces", "action", 0, cmd_network_interfaces},
    {"GetCapabilities", "action", 0, cmd_get_capabilities},
};

static const zcs_device_cmd k_onvif_media_cmds[] = {
    {"GetProfiles", "action", 0, cmd_get_profiles},
    {"GetVideoSources", "action", 0, cmd_video_sources},
    {"GetStreamUri", "action", 0, cmd_stream_uri},
    {"GetServiceCapabilities", "action", 0, cmd_media_service_capabilities},
};

static const zcs_device_cmd k_onvif_ptz_cmds[] = {
    {"GetServiceCapabilities", "action", 0, cmd_ptz_service_capabilities},
    {"GetNodes", "action", 0, cmd_ptz_get_nodes},
    {"GetConfigurations", "action", 0, cmd_ptz_get_configurations},
    {"GetConfigurationOptions", "action", 0, cmd_ptz_configuration_options},
    {"ContinuousMove", "action", 0, cmd_ptz_continuous_move},
    {"Stop", "action", 0, cmd_ptz_stop},
    {"GetStatus", "action", 0, cmd_ptz_get_status},
};

static const zcs_device_cmd k_onvif_events_cmds[] = {
    {"GetServiceCapabilities", "action", 0, cmd_events_service_capabilities},
    {"GetEventProperties", "action", 0, cmd_events_properties},
    {"CreatePullPointSubscription", "action", 0, cmd_create_pullpoint},
    {"PullMessages", "action", 0, cmd_pull_messages},
    {"Unsubscribe", "action", 0, cmd_unsubscribe},
    {"Renew", "action", 0, cmd_renew},
};

#define ONVIF_CMD_COUNT(t) (sizeof(t) / sizeof((t)[0]))

/*
 * 命令查找的兜底顺序：先按 SOAPAction 精确查表；查不到再扫 body 里是否出现
 * 表内的请求元素名。为什么需要第二步：不少 ONVIF 客户端（尤其部分国产 NVR）
 * 根本不发 SOAPAction 头或发得不准，只能靠 body 的根元素判断是哪个操作。
 */
static const zcs_device_cmd *onvif_find_cmd(const zcs_device_cmd *table, size_t count,
                                            const char *action, const char *body)
{
    const zcs_device_cmd *cmd = NULL;
    size_t i;

    if (action && action[0]) {
        cmd = zcs_device_cmd_find(table, count, action);
    }
    if (!cmd) {
        for (i = 0; i < count; ++i) {
            if (table[i].name && zcs_xml_find_tag_start(body, NULL, table[i].name)) {
                cmd = &table[i];
                break;
            }
        }
    }
    return cmd;
}

/*
 * ONVIF 请求总入口（poller 线程）。
 * 先取信封 Body 的第一个子元素名作为 action，再按 URL 路径选择命令表——
 * 同一路径前缀下复用一堆命令名（比如 GetServiceCapabilities 在每个服务里
 * 都要有），所以必须“路径定表、元素名定命令”两段式匹配。
 */
static void onvif_route_handle(zcs_http_session *sess, const zcs_http_request *req)
{
    const char *body = req && req->body ? req->body : "";

    if (!g_srv.enabled) {
        zcs_http_response_send_error(sess, 404, "Not Found");
        return;
    }
    if (!req || strcmp(req->method, "POST") != 0) {
        zcs_http_response_send_error(sess, 405, "Method Not Allowed");
        return;
    }

    {
        char action[80];
        const zcs_device_cmd *cmd;
        const zcs_device_cmd *table;
        size_t count;
        const char *svc;
        zcs_device_cmd_ctx ctx;

        if (zcs_xml_first_child_name(body, "Body", action, sizeof(action)) != 0) {
            action[0] = '\0';
        }
        if (strstr(req->path, "media_service") != NULL) {
            table = k_onvif_media_cmds;
            count = ONVIF_CMD_COUNT(k_onvif_media_cmds);
            svc = "media";
        } else if (strstr(req->path, "ptz_service") != NULL) {
            table = k_onvif_ptz_cmds;
            count = ONVIF_CMD_COUNT(k_onvif_ptz_cmds);
            svc = "ptz";
        } else if (strstr(req->path, "events_service") != NULL) {
            table = k_onvif_events_cmds;
            count = ONVIF_CMD_COUNT(k_onvif_events_cmds);
            svc = "events";
        } else {
            table = k_onvif_device_cmds;
            count = ONVIF_CMD_COUNT(k_onvif_device_cmds);
            svc = "device";
        }
        cmd = onvif_find_cmd(table, count, action, body);
        if (!cmd || !zcs_device_cmd_allowed(cmd)) {
            zn_info("onvif: unsupported %s action %s", svc, action[0] ? action : "?");
            respond_fault(sess, "ActionNotSupported", "unsupported action");
            return;
        }
        ctx.session = sess;
        ctx.message = body;
        ctx.extra = NULL;
        (void)cmd->exec(&ctx);
    }
}

static const zcs_http_route_ops k_onvif_route = {
    .name = "onvif",
    .match = onvif_route_match,
    .handle = onvif_route_handle,
};

void zcs_onvif_server_routes_register(void)
{
    if (g_routes_registered) {
        return;
    }
    g_routes_registered = 1;
    zcs_http_route_register(&k_onvif_route);
}

/* ── WS-Discovery 应答 ──────────────────────────────────────────────────── */

static void wsd_set_timeout(onvif_sock_t fd, int timeout_ms)
{
#if defined(_WIN32)
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

static size_t wsd_build_match(char *out, size_t cap, const char *relates_to)
{
    sb_t w;
    char dev[256];
    char scopes[512];
    char msgid[80];

    device_addr(dev, sizeof(dev), "device_service");
    zcs_onvif_make_msgid(msgid, sizeof(msgid));
    snprintf(scopes, sizeof(scopes),
             "onvif://www.onvif.org/name/%s onvif://www.onvif.org/hardware/ZCS "
             "onvif://www.onvif.org/type/NetworkVideoTransmitter",
             g_srv.name[0] ? g_srv.name : "ZCS");

    sb_init(&w, out, cap);
    sb_puts(&w,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\""
            " xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
            " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
            " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\"><e:Header>");
    sb_fmt(&w, "<w:MessageID>%s</w:MessageID>", msgid);
    if (relates_to && relates_to[0]) {
        sb_fmt(&w, "<w:RelatesTo>%s</w:RelatesTo>", relates_to);
    }
    sb_puts(&w,
            "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
            "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</w:Action>"
            "</e:Header><e:Body><d:ProbeMatches><d:ProbeMatch><w:EndpointReference>");
    sb_fmt(&w, "<w:Address>%s</w:Address>", g_srv.uuid);
    sb_puts(&w, "</w:EndpointReference><d:Types>dn:NetworkVideoTransmitter</d:Types>"
                "<d:Scopes>");
    sb_esc(&w, scopes);
    sb_puts(&w, "</d:Scopes><d:XAddrs>");
    sb_esc(&w, dev);
    sb_puts(&w, "</d:XAddrs><d:MetadataVersion>1</d:MetadataVersion>"
                "</d:ProbeMatch></d:ProbeMatches></e:Body></e:Envelope>");
    return w.len;
}

/* ── WS-Discovery：poller 驱动（无专用线程） ──────────────────────────────
 * 原实现是每进程一个线程 + 阻塞 recvfrom（500ms 超时）。模型只允许三类线程，
 * 这里改为把组播 socket 注册进 poller：可读事件进来后非阻塞收包并应答。
 * 顺带消除与原 HTTP poller 共享 g_srv 的竞态（现在同一线程访问）。 */

static zn_poller *g_wsd_poller;
static onvif_sock_t g_wsd_fd = ONVIF_SOCK_INVALID;
static int g_wsd_added;

static int wsd_sock_set_nonblock(onvif_sock_t fd)
{
#if defined(_WIN32)
    u_long nb = 1;
    return ioctlsocket(fd, FIONBIO, &nb) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/* 处理一条 Probe，必要时回 ProbeMatch。 */
/* 过滤逻辑：必须是 Probe 且不能是 ProbeMatches（否则会把自己的应答再当请求，
 * 造成组播回环风暴）。relates_to 回填请求的 MessageID，客户端据此配对。 */
static void wsd_handle_datagram(onvif_sock_t fd, char *buf, size_t len,
                                const struct sockaddr_in *peer, onvif_socklen_t plen)
{
    char reply[2048];
    char relates[80];
    size_t rlen;

    buf[len] = 0;
    if (!strstr(buf, "Probe") || strstr(buf, "ProbeMatches")) {
        return;
    }
    relates[0] = 0;
    (void)zcs_xml_text(buf, "MessageID", relates, sizeof(relates));
    rlen = wsd_build_match(reply, sizeof(reply), relates[0] ? relates : NULL);
#if defined(_WIN32)
    sendto(fd, reply, (int)rlen, 0, (const struct sockaddr *)peer, plen);
#else
    sendto(fd, reply, rlen, 0, (const struct sockaddr *)peer, plen);
#endif
}

/* poller 可读事件：非阻塞收包，单次上限 8 个数据报，避免饿死其它会话。 */
static void wsd_on_readable(int fd, unsigned events, void *user)
{
    int i;

    (void)events;
    (void)user;
    if (fd < 0) {
        return;
    }
    for (i = 0; i < 8; ++i) {
        char buf[2048];
        struct sockaddr_in peer;
        onvif_socklen_t plen = sizeof(peer);
        int n;

#if defined(_WIN32)
        n = recvfrom((onvif_sock_t)fd, buf, (int)sizeof(buf) - 1, 0, (struct sockaddr *)&peer,
                     &plen);
#else
        n = (int)recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&peer, &plen);
#endif
        if (n <= 0) {
            break; /* EAGAIN / 错误：等下一次可读 */
        }
        wsd_handle_datagram((onvif_sock_t)fd, buf, (size_t)n, &peer, plen);
    }
}

/* 在 poller 线程上建立组播 socket 并注册（跨线程投递）。 */
/* 通过 zn_poller_async 投递到 poller 执行，保证 socket 的创建/注册/收发
 * 全在同一线程，无需对 g_wsd_fd 加锁。ttl=2 是 ONVIF 设备常见值：足够覆盖
 * 同一交换域内的发现，又不至于把应答广播到过大的网段。 */
static void wsd_setup(void *user)
{
    onvif_sock_t fd;
    struct sockaddr_in addr;
    int ttl = 2;

    (void)user;
    if (!g_srv.discovery || g_wsd_fd != ONVIF_SOCK_INVALID || !g_wsd_poller) {
        return;
    }
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == ONVIF_SOCK_INVALID) {
        zn_warn("onvif: discovery socket failed");
        return;
    }
    {
        int reuse = 1;

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));
    }
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ONVIF_WSD_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        zn_warn("onvif: discovery bind 3702 failed");
        onvif_sock_close(fd);
        return;
    }
    {
        struct ip_mreq mreq;

        memset(&mreq, 0, sizeof(mreq));
        inet_pton(AF_INET, ONVIF_WSD_GROUP, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *)&mreq, sizeof(mreq)) != 0) {
            zn_warn("onvif: discovery join multicast failed");
        }
    }
    if (wsd_sock_set_nonblock(fd) != 0) {
        zn_warn("onvif: discovery set nonblock failed");
        onvif_sock_close(fd);
        return;
    }
    if (zn_poller_add(g_wsd_poller, (int)fd, ZN_POLL_IN | ZN_POLL_ERR, wsd_on_readable, NULL) !=
        ZN_OK) {
        zn_warn("onvif: discovery poller add failed");
        onvif_sock_close(fd);
        return;
    }
    g_wsd_fd = fd;
    g_wsd_added = 1;
    zn_info("onvif: discovery responder started (%s)", g_srv.uuid);
}

/*
 * 停止并释放 discovery socket。del_sync 是跨线程同步删除：调用方可能不在
 * poller 线程上，必须让 poller 完成 del 后再 close，否则 poller 可能正在
 * 该 fd 上等待可读，close 后继续回调造成 use-after-close。
 */
static void wsd_teardown(void)
{
    if (g_wsd_fd == ONVIF_SOCK_INVALID) {
        return;
    }
    if (g_wsd_added && g_wsd_poller) {
        /* 跨线程安全：poller 在运行则投递并等待完成。 */
        zn_poller_del_sync(g_wsd_poller, (int)g_wsd_fd);
    }
    onvif_sock_close(g_wsd_fd);
    g_wsd_fd = ONVIF_SOCK_INVALID;
    g_wsd_added = 0;
    zn_info("onvif: discovery responder stopped");
}

/*
 * 启动设备端。g_srv 是进程级单例：ONVIF 设备身份（uuid/主机/端口）全局只有一份，
 * 不支持多实例。WS-Discovery 的 socket 是在 poller 上异步建立的（zn_poller_async），
 * 这样 start 不必阻塞等待 bind/join 组播完成。
 */
int zcs_onvif_server_start(const zcs_onvif_server_opts *opts)
{
    if (!opts) {
        return -1;
    }
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.enabled = 1;
    snprintf(g_srv.host, sizeof(g_srv.host), "%s",
             opts->advertise_host && opts->advertise_host[0] ? opts->advertise_host : "127.0.0.1");
    g_srv.http_port = opts->http_port;
    g_srv.rtsp_port = opts->rtsp_port;
    snprintf(g_srv.name, sizeof(g_srv.name), "%s",
             opts->name && opts->name[0] ? opts->name : "ZCS");
    g_srv.discovery = opts->discovery;
    if (opts->uuid && opts->uuid[0]) {
        snprintf(g_srv.uuid, sizeof(g_srv.uuid), "%s", opts->uuid);
    } else {
        /* 未配置 uuid 时用单调时钟 + 固定卷积临时造一个：ONVIF 只要求 EndpointReference
         * 在会话内稳定、便于客户端去重，不要求跨重启持久。用单调时钟而非随机数是为了
         * 不引入 RNG 依赖；同一进程内多次启动基本不重复即可。 */
        uint8_t r[16];
        size_t i;
        for (i = 0; i < sizeof(r); ++i) {
            r[i] = (uint8_t)(zn_monotonic_ms() >> (i % 8) & 0xff) ^ (uint8_t)(i * 37u);
        }
        snprintf(g_srv.uuid, sizeof(g_srv.uuid),
                 "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12],
                 r[13], r[14], r[15]);
    }
    g_wsd_poller = opts->poller;
    if (g_srv.discovery && g_wsd_poller && g_wsd_fd == ONVIF_SOCK_INVALID) {
        if (zn_poller_async(g_wsd_poller, wsd_setup, NULL, 0) != ZN_OK) {
            zn_warn("onvif: schedule discovery setup failed");
        }
    }
    zn_info("onvif: device server enabled http=%u rtsp=%u host=%s", g_srv.http_port,
             g_srv.rtsp_port, g_srv.host);
    return 0;
}

void zcs_onvif_server_stop(void)
{
    wsd_teardown();
    g_wsd_poller = NULL;
    g_srv.enabled = 0;
}

int zcs_onvif_server_running(void)
{
    return g_srv.enabled;
}
