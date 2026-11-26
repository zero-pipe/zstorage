/**
 * @file gb_platform.c
 * @brief GB28181 平台端最小集：REGISTER/Digest/Keepalive/Catalog/DeviceInfo/状态查询。
 */
#include "zms/device/gb28181/gb28181_platform.h"

#include "device/gb28181/gb28181_internal.h"
#include "zms/protocol/rtp/ps_server.h"
#include "zms/device/core/device_event.h"
#include "zms/device/core/device_xml.h"
#include "zn/platform.h"
#include "zb/os/thread.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#if defined(_MSC_VER)
#define strcasecmp _stricmp
#endif
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif

typedef struct gb_pf_channel {
    char id[ZMS_GB_PLATFORM_ID_MAX];
    char name[128];
    int online;
} gb_pf_channel;

typedef struct gb_pf_session {
    char call_id[96];
    char device_id[ZMS_GB_PLATFORM_ID_MAX];
    char channel_id[ZMS_GB_PLATFORM_ID_MAX];
    char vhost[64];
    char app[64];
    char stream[128];
    char ssrc_txt[16];
    uint32_t ssrc;
    uint16_t local_port;
    int tcp;
    int active;
    char from_hdr[256];
    char to_hdr[256];
    char contact[256];
    uint32_t invite_cseq;
    uint64_t created_ms;
    zms_rtp_ps_server_slot *slot;
    struct gb_pf_session *next;
} gb_pf_session;

typedef struct gb_pf_record {
    char channel_id[ZMS_GB_PLATFORM_ID_MAX];
    char name[128];
    char file_path[512];
    char start_time[32];
    char end_time[32];
    char type[16];
} gb_pf_record;

typedef struct gb_pf_alarm {
    char device_id[ZMS_GB_PLATFORM_ID_MAX];
    char channel_id[ZMS_GB_PLATFORM_ID_MAX];
    int priority;
    int method;
    uint64_t time_ms;
    char description[128];
} gb_pf_alarm;

typedef struct gb_pf_device {
    char id[ZMS_GB_PLATFORM_ID_MAX];
    char ip[64];
    uint16_t port;
    int online;
    uint64_t last_seen_ms;
    int expires;
    char name[128];
    char manufacturer[64];
    char model[64];
    char firmware[32];
    gb_pf_channel channels[ZMS_GB_PLATFORM_CHANNEL_MAX];
    int channel_count;
    char rec_channel[ZMS_GB_PLATFORM_ID_MAX];
    gb_pf_record records[32];
    int record_count;
    char realm[64];
    char nonce[128];
    struct gb_pf_device *next;
} gb_pf_device;

static struct {
    int running;
    char id[ZMS_GB_PLATFORM_ID_MAX];
    char domain[ZMS_GB_PLATFORM_ID_MAX];
    char password[128];
    char advertise_ip[64];
    char transport[8];
    char tls_cert[512];
    char tls_key[512];
    char tls_ca[512];
    int tls_verify_peer;
    char media_vhost[64];
    char media_app[64];
    unsigned listen_port;
    int keepalive_timeout_sec;

    zn_poller_pool *pool;
    zn_poller *poller;
    zms_sip_transport *sip;
    zn_timer *tick;
    zb_mutex *mtx;
    gb_pf_device *devices;
    gb_pf_session *sessions;
    gb_pf_alarm alarms[32];
    size_t alarm_count;
    size_t alarm_idx;
    uint32_t cseq;
    uint32_t sn;
    uint32_t ssrc_seq;
} g_pf;

static void copy_str(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    snprintf(dst, cap, "%s", src ? src : "");
}

/*
 * 探测本机“面向某对端的本地 IP”。
 * 做法：建一个 UDP socket 并向对端 connect（不实际发包），再 getsockname 读
 * 内核为本方向选定的源地址。这是不用真发包、也不用遍历网卡就能拿到“能到达
 * 对端的网卡 IP”的经典技巧——多网卡/容器环境里比取第一个网卡可靠得多。
 * remote 非法或任何一步失败都退回 127.0.0.1（至少能本地自测）。
 */
static int detect_local_ip(const char *remote, char *out, size_t cap)
{
    struct sockaddr_in addr;
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
#if defined(_WIN32)
    SOCKET s;
#else
    int s;
#endif

    out[0] = '\0';
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    addr.sin_addr.s_addr = inet_addr(remote ? remote : "127.0.0.1");
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        copy_str(out, cap, "127.0.0.1");
        return -1;
    }
    s = socket(AF_INET, SOCK_DGRAM, 0);
#if defined(_WIN32)
    if (s == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        copy_str(out, cap, "127.0.0.1");
        return -1;
    }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        getsockname(s, (struct sockaddr *)&local, &len) != 0) {
#if defined(_WIN32)
        closesocket(s);
#else
        close(s);
#endif
        copy_str(out, cap, "127.0.0.1");
        return -1;
    }
    snprintf(out, cap, "%s", inet_ntoa(local.sin_addr));
#if defined(_WIN32)
    closesocket(s);
#else
    close(s);
#endif
    return 0;
}

static const char *local_ip_for(const char *peer)
{
    static char ip[64];

    if (g_pf.advertise_ip[0]) {
        return g_pf.advertise_ip;
    }
    if (detect_local_ip(peer, ip, sizeof(ip)) != 0) {
        copy_str(ip, sizeof(ip), "127.0.0.1");
    }
    return ip;
}

static const char *transport_name(void)
{
    if (strcasecmp(g_pf.transport, "tcp") == 0) {
        return "TCP";
    }
    if (strcasecmp(g_pf.transport, "tls") == 0) {
        return "TLS";
    }
    return "UDP";
}

/* ── 设备表 ─────────────────────────────────────────────────────────────── */

static gb_pf_device *dev_find_locked(const char *id)
{
    gb_pf_device *d;

    for (d = g_pf.devices; d; d = d->next) {
        if (strcmp(d->id, id) == 0) {
            return d;
        }
    }
    return NULL;
}

/* 按设备 ID 取或建。新建时给默认 Expires=3600 并头插进设备链表；调用方须持 mtx。
 * 设备是“来者即认”的模型：任何 REGISTER/Keepalive 提到的 ID 都会在这里落地，
 * 是否真正在线由后续字段更新决定。 */
static gb_pf_device *dev_get_or_create_locked(const char *id)
{
    gb_pf_device *d = dev_find_locked(id);

    if (d) {
        return d;
    }
    d = (gb_pf_device *)calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    copy_str(d->id, sizeof(d->id), id);
    d->expires = 3600;
    d->next = g_pf.devices;
    g_pf.devices = d;
    return d;
}

static void dev_clear_channels_locked(gb_pf_device *d)
{
    d->channel_count = 0;
    memset(d->channels, 0, sizeof(d->channels));
}

/* ── SIP 发送 ───────────────────────────────────────────────────────────── */

/* 统一回 SIP 响应：从 tx 缓冲池取一块、按本端对外 IP/端口拼响应、发出后归还。
 * 用连接池缓冲而非栈上大数组，是因为响应 body（如 Catalog）可能很大。 */
static void send_response_ex(const gb_sip_msg *m, int status, const char *reason,
                             const char *to_tag, const char *content_type, const void *body,
                             size_t body_len, const char *extra)
{
    char *buf = gb_sip_tx_alloc();
    int n;
    const char *lip = local_ip_for(m->peer_ip);

    if (!buf) {
        return;
    }
    n = gb_sip_build_response_ex(buf, GB_SIP_MSG_MAX, m, status, reason, lip, g_pf.listen_port,
                                 to_tag, g_pf.id, content_type, body, body_len, extra);
    if (n > 0) {
        (void)zms_sip_transport_send(g_pf.sip, m->peer_ip, m->peer_port, buf, (size_t)n);
    }
    free(buf);
}

/* 发 MESSAGE 查询（Catalog/DeviceInfo 等）。在锁内只取出目标设备地址并自增
 * SN/CSeq，拼包与发送放到锁外——device 指针出锁后不再解引用，避免长持有锁。 */
static int send_query(const char *device_id, const char *cmd)
{
    char body[1024];
    char uri[160];
    char call_id[96];
    char tag[64];
    char *buf;
    char ip[64];
    uint16_t port;
    uint32_t sn;
    unsigned cseq;
    gb_pf_device *d;
    gb_sip_req_spec spec;
    int n;
    gb_sb w;

    if (!g_pf.running || !device_id || !cmd) {
        return -1;
    }
    zb_mutex_lock(g_pf.mtx);
    d = dev_find_locked(device_id);
    if (!d) {
        zb_mutex_unlock(g_pf.mtx);
        return -1;
    }
    copy_str(ip, sizeof(ip), d->ip);
    port = d->port;
    sn = ++g_pf.sn;
    cseq = ++g_pf.cseq;
    zb_mutex_unlock(g_pf.mtx);

    gb_sb_init(&w, body, sizeof(body));
    gb_sb_puts(&w, "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Query>\r\n");
    gb_sb_printf(&w, "<CmdType>%s</CmdType>\r\n<SN>%u</SN>\r\n", cmd, (unsigned)sn);
    gb_sb_printf(&w, "<DeviceID>%s</DeviceID>\r\n</Query>\r\n", device_id);

    gb_rand_hex(call_id, 24);
    gb_rand_hex(tag, 12);
    snprintf(uri, sizeof(uri), "sip:%s@%s:%u", device_id, ip, port);
    memset(&spec, 0, sizeof(spec));
    spec.method = "MESSAGE";
    spec.uri = uri;
    spec.from_user = g_pf.id;
    spec.from_host = g_pf.domain;
    spec.from_tag = tag;
    spec.to_user = device_id;
    spec.to_host = g_pf.domain;
    spec.call_id = call_id;
    spec.cseq = cseq;
    spec.local_ip = local_ip_for(ip);
    spec.local_port = g_pf.listen_port;
    spec.transport = transport_name();
    spec.contact_user = g_pf.id;
    spec.content_type = "Application/MANSCDP+xml";
    spec.body = body;
    spec.body_len = strlen(body);
    buf = gb_sip_tx_alloc();
    if (!buf) {
        return -1;
    }
    n = gb_sip_build_request(buf, GB_SIP_MSG_MAX, &spec);
    if (n <= 0) {
        free(buf);
        return -1;
    }
    n = zms_sip_transport_send(g_pf.sip, ip, port, buf, (size_t)n);
    free(buf);
    return n;
}

/* ── MANSCDP 处理 ───────────────────────────────────────────────────────── */

/* 解析 Catalog 应答，用响应的 Item 列表整体替换该设备的通道集合。先清空再
 * 逐 Item 填：Catalog 是“全量快照”语义，不是增量，故不做合并。Status 缺省
 * 或为 ON 都算在线（部分设备不发 Status，按在线更实用）。 */
static void apply_catalog(gb_pf_device *d, const char *body)
{
    const char *p = body;
    int count = 0;

    dev_clear_channels_locked(d);
    while (p && *p) {
        const char *next;
        const char *item = zms_xml_find_tag_start(p, NULL, "Item");
        gb_pf_channel *ch;
        char tmp[64];

        if (!item) {
            break;
        }
        next = zms_xml_find_tag_start(item + 1, NULL, "Item");
        if (count >= ZMS_GB_PLATFORM_CHANNEL_MAX) {
            break;
        }
        ch = &d->channels[count];
        memset(ch, 0, sizeof(*ch));
        (void)zms_xml_text_between(item, next, "DeviceID", ch->id, sizeof(ch->id));
        if (!ch->id[0]) {
            break;
        }
        (void)zms_xml_text_between(item, next, "Name", ch->name, sizeof(ch->name));
        if (!ch->name[0]) {
            copy_str(ch->name, sizeof(ch->name), ch->id);
        }
        tmp[0] = '\0';
        (void)zms_xml_text_between(item, next, "Status", tmp, sizeof(tmp));
        ch->online = (tmp[0] == '\0' || strcasecmp(tmp, "ON") == 0);
        ++count;
        if (!next) {
            break;
        }
        p = next;
    }
    d->channel_count = count;
    zn_info("gb_platform: catalog device=%s channels=%d", d->id, count);
}

static void apply_records(gb_pf_device *d, const char *body)
{
    const char *p = body;
    int count = 0;
    char channel[ZMS_GB_PLATFORM_ID_MAX] = "";

    (void)zms_xml_text(body, "DeviceID", channel, sizeof(channel));
    copy_str(d->rec_channel, sizeof(d->rec_channel), channel);
    d->record_count = 0;
    while (p && *p) {
        const char *item = zms_xml_find_tag_start(p, NULL, "Item");
        const char *next;
        gb_pf_record *r;

        if (!item) {
            break;
        }
        next = zms_xml_find_tag_start(item + 1, NULL, "Item");
        if (count >= (int)(sizeof(d->records) / sizeof(d->records[0]))) {
            break;
        }
        r = &d->records[count];
        memset(r, 0, sizeof(*r));
        copy_str(r->channel_id, sizeof(r->channel_id), channel);
        (void)zms_xml_text_between(item, next, "Name", r->name, sizeof(r->name));
        (void)zms_xml_text_between(item, next, "FilePath", r->file_path, sizeof(r->file_path));
        (void)zms_xml_text_between(item, next, "StartTime", r->start_time, sizeof(r->start_time));
        (void)zms_xml_text_between(item, next, "EndTime", r->end_time, sizeof(r->end_time));
        (void)zms_xml_text_between(item, next, "Type", r->type, sizeof(r->type));
        ++count;
        if (!next) {
            break;
        }
        p = next;
    }
    d->record_count = count;
    zn_info("gb_platform: records device=%s channel=%s count=%d", d->id, channel, count);
}

/*
 * 处理设备主动上报的 MESSAGE（Keepalive / Catalog / DeviceInfo / Alarm 等）。
 * 先无条件回 200，再解析 body——MANSCDP 的语义是“收到即确认”，解析失败也不
 * 回错误码。设备已存在则刷新 last_seen；不存在则借 Keepalive 自动补登记（见
 * 函数内注释）。锁内只改设备表，闭环外的 send_query / 事件发布放到锁外，避免
 * 持锁做 IO 或触发其他模块回调导致死锁。
 */
static void handle_device_message(const gb_sip_msg *m)
{
    gb_manscdp_msg q;
    char device_id[ZMS_GB_PLATFORM_ID_MAX] = "";
    gb_pf_device *d;
    int auto_created = 0;

    send_response_ex(m, 200, "OK", NULL, NULL, NULL, 0, NULL);
    gb_sip_uri_user(gb_sip_hdr(m, "From"), device_id, sizeof(device_id));
    if (!device_id[0]) {
        return;
    }
    if (gb_manscdp_parse(m->body, m->body_len, &q) != 0) {
        return;
    }
    zb_mutex_lock(g_pf.mtx);
    d = dev_find_locked(device_id);
    if (d) {
        d->last_seen_ms = zn_monotonic_ms();
        d->online = 1;
    } else {
        /* 平台重启后相机仍认为注册有效（Expires 未到期），只发心跳不重发 REGISTER：
         * 用 Keepalive 自动补登记，避免设备列表长期为空。 */
        d = dev_get_or_create_locked(device_id);
        if (d) {
            copy_str(d->ip, sizeof(d->ip), m->peer_ip);
            d->port = m->peer_port;
            d->online = 1;
            d->last_seen_ms = zn_monotonic_ms();
            zn_info("gb_platform: auto-register on keepalive id=%s from %s:%u", device_id,
                    m->peer_ip, (unsigned)m->peer_port);
            auto_created = 1;
        }
    }
    zb_mutex_unlock(g_pf.mtx);
    if (!d) {
        return;
    }
    if (auto_created) {
        (void)send_query(device_id, "Catalog");
        (void)send_query(device_id, "DeviceInfo");
    }

    if (strcmp(q.root, "Notify") == 0 && strcmp(q.cmd_type, "Keepalive") == 0) {
        zn_debug("gb_platform: keepalive device=%s", device_id);
        return;
    }
    if (strcmp(q.root, "Response") == 0) {
        zb_mutex_lock(g_pf.mtx);
        if (strcmp(q.cmd_type, "Catalog") == 0) {
            apply_catalog(d, m->body);
        } else if (strcmp(q.cmd_type, "DeviceInfo") == 0) {
            (void)zms_xml_text(m->body, "DeviceName", d->name, sizeof(d->name));
            (void)zms_xml_text(m->body, "Manufacturer", d->manufacturer,
                               sizeof(d->manufacturer));
            (void)zms_xml_text(m->body, "Model", d->model, sizeof(d->model));
            (void)zms_xml_text(m->body, "Firmware", d->firmware, sizeof(d->firmware));
            zn_info("gb_platform: device info id=%s name=%s", d->id, d->name);
        } else if (strcmp(q.cmd_type, "DeviceStatus") == 0) {
            char tmp[64] = "";
            (void)zms_xml_text(m->body, "Online", tmp, sizeof(tmp));
            d->online = (tmp[0] == '\0' || strcasecmp(tmp, "ONLINE") == 0);
        } else if (strcmp(q.cmd_type, "RecordInfo") == 0) {
            apply_records(d, m->body);
        }
        zb_mutex_unlock(g_pf.mtx);
        return;
    }
    if (strcmp(q.root, "Notify") == 0 && strcmp(q.cmd_type, "Alarm") == 0) {
        zms_device_event ev;
        char pri[16] = "";
        char method[16] = "";
        char desc[128] = "";
        gb_pf_alarm *a;

        (void)zms_xml_text(m->body, "AlarmPriority", pri, sizeof(pri));
        (void)zms_xml_text(m->body, "AlarmMethod", method, sizeof(method));
        (void)zms_xml_text(m->body, "AlarmDescription", desc, sizeof(desc));
        /* 报警用固定 32 槽环形缓冲：报警可能高频且无需全量保留，只需要“最近
         * 若干条”供查询。写满后覆盖最旧的一条，alarm_idx 单调递增用于定位。 */
        zb_mutex_lock(g_pf.mtx);
        a = &g_pf.alarms[g_pf.alarm_idx % 32];
        g_pf.alarm_idx++;
        if (g_pf.alarm_count < 32) {
            g_pf.alarm_count++;
        }
        memset(a, 0, sizeof(*a));
        copy_str(a->device_id, sizeof(a->device_id), device_id);
        copy_str(a->channel_id, sizeof(a->channel_id), q.device_id);
        a->priority = atoi(pri);
        a->method = atoi(method);
        a->time_ms = zn_wall_ms();
        copy_str(a->description, sizeof(a->description), desc);
        zb_mutex_unlock(g_pf.mtx);
        zn_info("gb_platform: alarm device=%s channel=%s priority=%s", device_id, q.device_id,
                 pri);
        memset(&ev, 0, sizeof(ev));
        ev.type = ZMS_DEV_EVENT_ALARM;
        copy_str(ev.device_id, sizeof(ev.device_id), device_id);
        copy_str(ev.channel_id, sizeof(ev.channel_id), q.device_id);
        ev.priority = a->priority;
        copy_str(ev.message, sizeof(ev.message), desc);
        zms_device_event_publish(&ev);
    }
}

/* Expires 优先级：Expires 头 → Contact 里的 expires 参数 → 默认 3600。
 * 设备实现不统一，两级来源都尝试，保证不会因缺一个头就把有效期当成 0。 */
static int parse_expires(const gb_sip_msg *m)
{
    const char *e = gb_sip_hdr(m, "Expires");
    char tmp[32];

    if (e && e[0]) {
        return atoi(e);
    }
    tmp[0] = '\0';
    gb_sip_param(gb_sip_hdr(m, "Contact"), "expires", tmp, sizeof(tmp));
    if (tmp[0]) {
        return atoi(tmp);
    }
    return 3600;
}

/*
 * REGISTER 处理，含 Digest 认证三态：
 *   1. 无 Authorization → 回 401 + WWW-Authenticate（带 realm/nonce），请设备带
 *      摘要重发；nonce 首次生成后缓存在设备记录上，重发时复用。
 *   2. 有 Authorization 但校验失败 → 403 拒绝。
 *   3. 校验通过（或未配置密码，即开放注册）→ 记录地址/online/expires 并 200。
 * Expires=0 是注销：标离线并广播 OFFLINE，回 200 带 Expires: 0。
 * 与 401 的相对顺序也重要：先确认设备、生成 nonce，再发 challenge。
 */
static void handle_register(const gb_sip_msg *m)
{
    char device_id[ZMS_GB_PLATFORM_ID_MAX] = "";
    char from_user[ZMS_GB_PLATFORM_ID_MAX] = "";
    char challenge[512];
    char extra[256];
    int expires = parse_expires(m);
    gb_pf_device *d;

    gb_sip_uri_user(gb_sip_hdr(m, "From"), from_user, sizeof(from_user));
    copy_str(device_id, sizeof(device_id), from_user);
    if (!device_id[0]) {
        send_response_ex(m, 400, "Bad Request", NULL, NULL, NULL, 0, NULL);
        return;
    }

    zb_mutex_lock(g_pf.mtx);
    d = dev_get_or_create_locked(device_id);
    if (!d) {
        zb_mutex_unlock(g_pf.mtx);
        send_response_ex(m, 500, "Server Error", NULL, NULL, NULL, 0, NULL);
        return;
    }
    if (expires == 0) {
        d->online = 0;
        d->port = 0;
        zb_mutex_unlock(g_pf.mtx);
        snprintf(extra, sizeof(extra), "Expires: 0\r\n");
        send_response_ex(m, 200, "OK", "pf", NULL, NULL, 0, extra);
        zn_info("gb_platform: unregister device=%s", device_id);
        {
            zms_device_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ZMS_DEV_EVENT_OFFLINE;
            copy_str(ev.device_id, sizeof(ev.device_id), device_id);
            zms_device_event_publish(&ev);
        }
        return;
    }
    if (!d->nonce[0]) {
        gb_rand_hex(d->nonce, 32);
    }
    copy_str(d->realm, sizeof(d->realm), g_pf.domain);
    zb_mutex_unlock(g_pf.mtx);

    if (g_pf.password[0]) {
        const char *auth = gb_sip_hdr(m, "Authorization");
        if (!auth) {
            snprintf(challenge, sizeof(challenge),
                     "WWW-Authenticate: Digest realm=\"%s\", nonce=\"%s\", algorithm=MD5\r\n",
                     g_pf.domain, d->nonce);
            zn_debug("gb_platform: challenge device=%s", device_id);
            send_response_ex(m, 401, "Unauthorized", "pf", NULL, NULL, 0, challenge);
            return;
        }
        if (!gb_digest_verify(auth, "REGISTER", m->uri, g_pf.domain, d->nonce, device_id,
                              g_pf.password)) {
            zn_warn("gb_platform: register auth failed device=%s", device_id);
            send_response_ex(m, 403, "Forbidden", "pf", NULL, NULL, 0, NULL);
            return;
        }
    }

    zb_mutex_lock(g_pf.mtx);
    d = dev_find_locked(device_id);
    if (!d) {
        zb_mutex_unlock(g_pf.mtx);
        return;
    }
    copy_str(d->ip, sizeof(d->ip), m->peer_ip);
    d->port = m->peer_port;
    d->online = 1;
    d->last_seen_ms = zn_monotonic_ms();
    d->expires = expires;
    zb_mutex_unlock(g_pf.mtx);

    snprintf(extra, sizeof(extra), "Expires: %d\r\n", expires);
    send_response_ex(m, 200, "OK", "pf", NULL, NULL, 0, extra);
    zn_info("gb_platform: registered device=%s %s:%u expires=%d", device_id, m->peer_ip,
             m->peer_port, expires);
    {
        zms_device_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ZMS_DEV_EVENT_ONLINE;
        copy_str(ev.device_id, sizeof(ev.device_id), device_id);
        zms_device_event_publish(&ev);
    }

    (void)send_query(device_id, "Catalog");
    (void)send_query(device_id, "DeviceInfo");
}

static void session_remove_locked(const char *call_id);
static void handle_invite_response(const gb_sip_msg *m);

/* ── 传输回调 / 定时器 ─────────────────────────────────────────────────── */

/*
 * SIP 消息总入口（传输层收到完整消息后回调）。按类型分派：
 *   - 响应：按 CSeq 里的方法名分到 INVITE/BYE 处理；
 *   - 请求：REGISTER/MESSAGE 各自处理；OPTIONS/SUBSCRIBE 直接 200；
 *   - 其余 405。
 * 每条消息 calloc 在堆上（body 可达 ~50KB），避免压满 poller 线程栈。
 * 注意 parse 后先判空方法/非响应：海康用空 UDP 包做 NAT 保活，需静默忽略。
 */
static void pf_on_message(void *user, const char *peer_ip, uint16_t peer_port, const void *data,
                          size_t len)
{
    /* 单条消息 ~50KB：放堆上，避免把 poller 线程栈压到百 KB 级。 */
    gb_sip_msg *m = (gb_sip_msg *)calloc(1, sizeof(*m));

    (void)user;
    if (!m) {
        return;
    }
    if (gb_sip_parse(data, len, peer_ip, peer_port, m) != 0) {
        free(m);
        zn_warn("gb_platform: bad SIP message from %s:%u", peer_ip, peer_port);
        return;
    }
    if (!m->method[0] && !m->is_response) {
        /* 部分相机（海康）用空 UDP 包做 NAT 保活：静默忽略，不要回 405 */
        zn_debug("gb_platform: ignore empty SIP datagram from %s:%u", peer_ip, peer_port);
        free(m);
        return;
    }
    if (m->is_response) {
        const char *cseq = gb_sip_hdr(m, "CSeq");
        if (cseq && strstr(cseq, "INVITE")) {
            handle_invite_response(m);
        } else if (cseq && strstr(cseq, "BYE")) {
            const char *call_id = gb_sip_hdr(m, "Call-ID");
            if (call_id && m->status == 200) {
                zb_mutex_lock(g_pf.mtx);
                session_remove_locked(call_id);
                zb_mutex_unlock(g_pf.mtx);
            }
        }
        free(m);
        return;
    }
    if (strcmp(m->method, "REGISTER") == 0) {
        zn_debug("gb_platform: REGISTER from %s:%u auth=%d", peer_ip, peer_port,
                  gb_sip_hdr(m, "Authorization") ? 1 : 0);
        handle_register(m);
    } else if (strcmp(m->method, "MESSAGE") == 0) {
        handle_device_message(m);
    } else if (strcmp(m->method, "OPTIONS") == 0 || strcmp(m->method, "SUBSCRIBE") == 0) {
        send_response_ex(m, 200, "OK", NULL, NULL, NULL, 0, NULL);
    } else {
        send_response_ex(m, 405, "Method Not Allowed", NULL, NULL, NULL, 0, NULL);
    }
    free(m);
}

/*
 * 周期 tick（1s）：清理两类“僵尸”状态。
 *   1. INVITE 发出后 10s 仍未 active：视为失败，移除会话并释放其 RTP-PS 槽位，
 *      否则端口/媒体源会一直被一个没建起来的会话占着。
 *   2. 在线设备超过 keepalive 超时（或超过其 Expires）：标离线并广播 OFFLINE
 *      事件。两个阈值取“谁先到算谁”，因为设备可能心跳很长但 REGISTER 的
 *      Expires 很短（或反之），取更严格的那个更安全。
 * 整个遍历持 g_pf.mtx，因为会话/设备链表由多个回调线程共享。
 */
static void pf_tick(void *user)
{
    uint64_t now = zn_monotonic_ms();
    gb_pf_device *d;

    (void)user;
    zb_mutex_lock(g_pf.mtx);
    {
        gb_pf_session *s = g_pf.sessions;
        while (s) {
            gb_pf_session *next = s->next;
            if (!s->active && now - s->created_ms > 10000u) {
                zn_warn("gb_platform: INVITE timeout call=%s", s->call_id);
                session_remove_locked(s->call_id);
            }
            s = next;
        }
    }
    for (d = g_pf.devices; d; d = d->next) {
        uint64_t timeout = (uint64_t)g_pf.keepalive_timeout_sec * 1000u;
        uint64_t expires = (uint64_t)(d->expires > 0 ? d->expires : 3600) * 1000u;
        if (d->online && (now - d->last_seen_ms > timeout || now - d->last_seen_ms > expires)) {
            int i;
            d->online = 0;
            for (i = 0; i < d->channel_count; ++i) {
                d->channels[i].online = 0;
            }
            zn_warn("gb_platform: device offline (timeout) id=%s", d->id);
            {
                zms_device_event ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = ZMS_DEV_EVENT_OFFLINE;
                copy_str(ev.device_id, sizeof(ev.device_id), d->id);
                zms_device_event_publish(&ev);
            }
        }
    }
    zb_mutex_unlock(g_pf.mtx);
}

static int send_manscdp_body(const char *device_id, const char *body)
{
    char uri[160];
    char call_id[96];
    char tag[64];
    char *buf;
    char ip[64];
    uint16_t port;
    unsigned cseq;
    gb_pf_device *d;
    gb_sip_req_spec spec;
    int n;

    if (!g_pf.running || !device_id || !body) {
        return -1;
    }
    zb_mutex_lock(g_pf.mtx);
    d = dev_find_locked(device_id);
    if (!d) {
        zb_mutex_unlock(g_pf.mtx);
        return -1;
    }
    copy_str(ip, sizeof(ip), d->ip);
    port = d->port;
    cseq = ++g_pf.cseq;
    zb_mutex_unlock(g_pf.mtx);

    gb_rand_hex(call_id, 24);
    gb_rand_hex(tag, 12);
    snprintf(uri, sizeof(uri), "sip:%s@%s:%u", device_id, ip, port);
    memset(&spec, 0, sizeof(spec));
    spec.method = "MESSAGE";
    spec.uri = uri;
    spec.from_user = g_pf.id;
    spec.from_host = g_pf.domain;
    spec.from_tag = tag;
    spec.to_user = device_id;
    spec.to_host = g_pf.domain;
    spec.call_id = call_id;
    spec.cseq = cseq;
    spec.local_ip = local_ip_for(ip);
    spec.local_port = g_pf.listen_port;
    spec.transport = transport_name();
    spec.contact_user = g_pf.id;
    spec.content_type = "Application/MANSCDP+xml";
    spec.body = body;
    spec.body_len = strlen(body);
    buf = gb_sip_tx_alloc();
    if (!buf) {
        return -1;
    }
    n = gb_sip_build_request(buf, GB_SIP_MSG_MAX, &spec);
    if (n <= 0) {
        free(buf);
        return -1;
    }
    n = zms_sip_transport_send(g_pf.sip, ip, port, buf, (size_t)n);
    free(buf);
    return n;
}

/*
 * 构造 GB28181 云台命令（A5 0F 01 ... 格式）。pan/tilt/zoom 归一到 [-1,1]，
 * 速度取绝对值 ×100（0-100）。八方向 + 变倍由不同的 cmd 字节区分；
 * stop 或全零一律发 0x00（停）。末尾是校验和：前 7 字节累加取低 8 位，
 * 设备端据此判定命令完整——校验错设备会静默丢弃，所以这步不能省。
 */
static void build_ptz_cmd(float pan, float tilt, float zoom, int stop, char out[17])
{
    int ps = 0;
    int ts = 0;
    int zs = 0;
    int cmd = 0;
    int check;

    pan = pan > 1.0f ? 1.0f : (pan < -1.0f ? -1.0f : pan);
    tilt = tilt > 1.0f ? 1.0f : (tilt < -1.0f ? -1.0f : tilt);
    zoom = zoom > 1.0f ? 1.0f : (zoom < -1.0f ? -1.0f : zoom);
    ps = (int)((pan < 0 ? -pan : pan) * 100.0f);
    ts = (int)((tilt < 0 ? -tilt : tilt) * 100.0f);
    zs = (int)((zoom < 0 ? -zoom : zoom) * 100.0f);

    if (stop || (pan == 0.0f && tilt == 0.0f && zoom == 0.0f)) {
        cmd = 0x00;
    } else if (pan == 0.0f && tilt == 0.0f) {
        cmd = zoom > 0 ? 0x09 : 0x0A;
    } else if (pan < 0 && tilt > 0) {
        cmd = 0x05;
    } else if (pan > 0 && tilt > 0) {
        cmd = 0x06;
    } else if (pan < 0 && tilt < 0) {
        cmd = 0x07;
    } else if (pan > 0 && tilt < 0) {
        cmd = 0x08;
    } else if (pan < 0) {
        cmd = 0x01;
    } else if (pan > 0) {
        cmd = 0x02;
    } else if (tilt > 0) {
        cmd = 0x03;
    } else if (tilt < 0) {
        cmd = 0x04;
    }
    check = (0xA5 + 0x0F + 0x01 + cmd + ps + ts + zs) % 256;
    snprintf(out, 17, "A50F01%02X%02X%02X%02X%02X", cmd, ps & 0xFF, ts & 0xFF, zs & 0xFF,
             check);
}

int zms_gb28181_platform_ptz(const char *device_id, const char *channel_id, float pan,
                             float tilt, float zoom, int stop)
{
    char ptz[24];
    char body[1024];
    uint32_t sn;

    if (!device_id || !channel_id) {
        return -1;
    }
    zb_mutex_lock(g_pf.mtx);
    sn = ++g_pf.sn;
    zb_mutex_unlock(g_pf.mtx);
    build_ptz_cmd(pan, tilt, zoom, stop, ptz);
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Control>\r\n"
             "<CmdType>DeviceControl</CmdType>\r\n<SN>%u</SN>\r\n"
             "<DeviceID>%s</DeviceID>\r\n<PTZCmd>%s</PTZCmd>\r\n</Control>\r\n",
             (unsigned)sn, channel_id, ptz);
    zn_info("gb_platform: PTZ device=%s channel=%s cmd=%s", device_id, channel_id, ptz);
    return send_manscdp_body(device_id, body);
}

int zms_gb28181_platform_record_query(const char *device_id, const char *channel_id,
                                      const char *start_time, const char *end_time)
{
    char body[1024];
    uint32_t sn;

    if (!device_id || !channel_id || !start_time || !end_time) {
        return -1;
    }
    zb_mutex_lock(g_pf.mtx);
    sn = ++g_pf.sn;
    zb_mutex_unlock(g_pf.mtx);
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n<Query>\r\n"
             "<CmdType>RecordInfo</CmdType>\r\n<SN>%u</SN>\r\n"
             "<DeviceID>%s</DeviceID>\r\n<StartTime>%s</StartTime>\r\n"
             "<EndTime>%s</EndTime>\r\n<Secrecy>0</Secrecy>\r\n<Type>all</Type>\r\n"
             "</Query>\r\n",
             (unsigned)sn, channel_id, start_time, end_time);
    return send_manscdp_body(device_id, body);
}

/* 对外查询：把设备最近一次 RecordInfo 的结果拷出（可按 channel_id 过滤，NULL/空
 * 表示全部）。返回实际拷贝条数，受 cap 限制。持锁拷贝，出锁即与内部状态脱钩。 */
size_t zms_gb28181_platform_records(const char *device_id, const char *channel_id,
                                    zms_gb28181_platform_record *out, size_t cap)
{
    gb_pf_device *d;
    size_t n = 0;
    int i;

    if (!device_id || !out || cap == 0 || !g_pf.mtx) {
        return 0;
    }
    zb_mutex_lock(g_pf.mtx);
    d = dev_find_locked(device_id);
    if (d) {
        for (i = 0; i < d->record_count && n < cap; ++i) {
            zms_gb28181_platform_record *o;
            if (channel_id && channel_id[0] && strcmp(d->records[i].channel_id, channel_id) != 0) {
                continue;
            }
            o = &out[n++];
            memset(o, 0, sizeof(*o));
            copy_str(o->channel_id, sizeof(o->channel_id), d->records[i].channel_id);
            copy_str(o->name, sizeof(o->name), d->records[i].name);
            copy_str(o->file_path, sizeof(o->file_path), d->records[i].file_path);
            copy_str(o->start_time, sizeof(o->start_time), d->records[i].start_time);
            copy_str(o->end_time, sizeof(o->end_time), d->records[i].end_time);
            copy_str(o->type, sizeof(o->type), d->records[i].type);
        }
    }
    zb_mutex_unlock(g_pf.mtx);
    return n;
}

/* 报警查询：从环形缓冲里按“最新在前”的顺序取。alarm_idx 是下一个待写位置，
 * 故最新一条在 (idx-1)，用 +32 避免下溢，逐条往回取 i 个。 */
size_t zms_gb28181_platform_alarms(zms_gb28181_platform_alarm *out, size_t cap)
{
    size_t n;
    size_t i;

    if (!out || cap == 0 || !g_pf.mtx) {
        return 0;
    }
    zb_mutex_lock(g_pf.mtx);
    n = g_pf.alarm_count < cap ? g_pf.alarm_count : cap;
    for (i = 0; i < n; ++i) {
        size_t idx = (g_pf.alarm_idx + 32 - 1 - i) % 32;
        const gb_pf_alarm *a = &g_pf.alarms[idx];
        memset(&out[i], 0, sizeof(out[i]));
        copy_str(out[i].device_id, sizeof(out[i].device_id), a->device_id);
        copy_str(out[i].channel_id, sizeof(out[i].channel_id), a->channel_id);
        out[i].priority = a->priority;
        out[i].method = a->method;
        out[i].time_ms = a->time_ms;
        copy_str(out[i].description, sizeof(out[i].description), a->description);
    }
    zb_mutex_unlock(g_pf.mtx);
    return n;
}

/* ── INVITE 取流（M4b） ────────────────────────────────────────────────── */

static gb_pf_session *session_find_locked(const char *call_id)
{
    gb_pf_session *s;

    for (s = g_pf.sessions; s; s = s->next) {
        if (strcmp(s->call_id, call_id) == 0) {
            return s;
        }
    }
    return NULL;
}

static void session_free(gb_pf_session *s)
{
    if (!s) {
        return;
    }
    if (s->slot) {
        zms_rtp_ps_server_close(s->slot);
        s->slot = NULL;
    }
    free(s);
}

/*
 * 从会话链表摘除并释放一个会话。调用方必须持 g_pf.mtx（函数名 _locked 即此约定）。
 * 注意这里会顺带关闭会话持有的 RTP-PS 槽位——这步可能触及媒体层的资源释放，
 * 因此只在信令线程的少数路径（tick 超时/BYE 响应/发送失败回滚）调用，且都在
 * 明确的“该销毁”时机，不会与媒体线程争用同一槽位的生命周期。
 */
static void session_remove_locked(const char *call_id)
{
    gb_pf_session **pp;

    for (pp = &g_pf.sessions; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->call_id, call_id) == 0) {
            gb_pf_session *s = *pp;
            *pp = s->next;
            s->next = NULL;
            /* 槽位关闭不能在锁内做（可能回调媒体线程），交给调用方 */
            {
                zms_rtp_ps_server_slot *slot = s->slot;
                s->slot = NULL;
                if (slot) {
                    zms_rtp_ps_server_close(slot);
                }
            }
            free(s);
            return;
        }
    }
}

/*
 * 生成 SSRC（10 位十进制串），同时给出数值形式。给 device 的 INVITE SDP 里
 * 用它作为 y= 行，媒体通道按它归属。拼法：前导 '0' + 域名的中间 5 位 + 自增
 * 序号（模 10000），近似 GB28181 约定的“10 位、含行政区划+序号”格式，且保证
 * 同一平台内短时间内不重号。
 */
static void make_ssrc(char *txt, size_t cap, uint32_t *num)
{
    char part[8] = "";

    if (strlen(g_pf.domain) >= 8) {
        snprintf(part, sizeof(part), "%.5s", g_pf.domain + 3);
    } else {
        snprintf(part, sizeof(part), "%s", g_pf.domain);
    }
    snprintf(txt, cap, "0%s%04u", part, (unsigned)(++g_pf.ssrc_seq % 10000u));
    *num = (uint32_t)strtoul(txt, NULL, 10);
}

/*
 * 解析设备端 answer SDP 里需要的最小三元组：媒体连接地址（c=IN IP4）、
 * 视频端口（m=video 后的端口）、setup 角色。逐行扫描而非完整 SDP 解析，
 * 因为这里只关心这三项，全量解析反而引入不必要的依赖与开销。
 */
static void sdp_parse_answer(const char *sdp, char *ip, size_t ip_cap, uint16_t *port,
                             char *setup, size_t setup_cap)
{
    const char *p = sdp;

    ip[0] = '\0';
    *port = 0;
    setup[0] = '\0';
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];

        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        if (line[0] && line[n - 1] == '\r') {
            line[n - 1] = '\0';
        }
        if (strncmp(line, "c=IN IP4 ", 9) == 0) {
            copy_str(ip, ip_cap, line + 9);
        } else if (strncmp(line, "m=video ", 8) == 0) {
            unsigned v = 0;
            if (sscanf(line + 8, "%u", &v) == 1) {
                *port = (uint16_t)v;
            }
        } else if (strncmp(line, "a=setup:", 8) == 0) {
            copy_str(setup, setup_cap, line + 8);
        }
        p = eol ? eol + 1 : NULL;
    }
}

static int send_ack(const gb_pf_session *sess, const gb_sip_msg *resp)
{
    char buf[4096];
    gb_sb w;
    char branch[32];
    const char *lip = local_ip_for(resp->peer_ip);

    gb_rand_hex(branch, 20);
    gb_sb_init(&w, buf, sizeof(buf));
    gb_sb_printf(&w, "ACK %s SIP/2.0\r\n", sess->contact[0] ? sess->contact : sess->channel_id);
    gb_sb_printf(&w, "Via: SIP/2.0/%s %s:%u;rport;branch=z9hG4bK%s\r\n", transport_name(), lip,
                 g_pf.listen_port, branch);
    gb_sb_printf(&w, "From: %s\r\n", sess->from_hdr);
    gb_sb_printf(&w, "To: %s\r\n", sess->to_hdr);
    gb_sb_printf(&w, "Call-ID: %s\r\n", sess->call_id);
    gb_sb_printf(&w, "CSeq: %u ACK\r\n", (unsigned)sess->invite_cseq);
    gb_sb_puts(&w, "Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n");
    return zms_sip_transport_send(g_pf.sip, resp->peer_ip, resp->peer_port, buf, w.len);
}

/*
 * INVITE 200 OK 处理：从 answer 的 SDP 里取媒体地址/端口/setup，回 ACK。
 * 关键分支在 sess->tcp==2（TCP-ACTIVE）：此时设备告诉我们它监听的 ip:port，
 * 我们必须主动连过去（zms_rtp_ps_server_connect）。UDP / TCP-PASSIVE 不用连，
 * 前者靠发、后者靠监听。Contact 里的 <uri> 存下来供后续 BYE 复用。
 */
static void handle_invite_response(const gb_sip_msg *m)
{
    const char *call_id = gb_sip_hdr(m, "Call-ID");
    gb_pf_session *sess;

    if (!call_id) {
        return;
    }
    zb_mutex_lock(g_pf.mtx);
    sess = session_find_locked(call_id);
    zb_mutex_unlock(g_pf.mtx);
    if (!sess) {
        return;
    }
    if (m->status >= 300) {
        zn_warn("gb_platform: INVITE failed %d call=%s", m->status, call_id);
        zb_mutex_lock(g_pf.mtx);
        session_remove_locked(call_id);
        zb_mutex_unlock(g_pf.mtx);
        return;
    }
    if (m->status != 200) {
        return;
    }
    {
        char ip[64];
        char setup[32];
        uint16_t port = 0;
        const char *contact = gb_sip_hdr(m, "Contact");
        const char *to = gb_sip_hdr(m, "To");
        char uri[256] = "";

        sdp_parse_answer(m->body, ip, sizeof(ip), &port, setup, sizeof(setup));
        if (contact) {
            const char *lt = strchr(contact, '<');
            const char *gt = lt ? strchr(lt, '>') : NULL;
            if (lt && gt && (size_t)(gt - lt - 1) < sizeof(uri)) {
                memcpy(uri, lt + 1, (size_t)(gt - lt - 1));
                uri[gt - lt - 1] = '\0';
            }
        }
        sess->active = 1;
        copy_str(sess->contact, sizeof(sess->contact), uri);
        copy_str(sess->to_hdr, sizeof(sess->to_hdr), to ? to : "");
        if (sess->tcp == 2 && ip[0] && port) {
            if (zms_rtp_ps_server_connect(g_pf.poller, sess->vhost, sess->app, sess->stream, ip,
                                          port) != ZN_OK) {
                zn_warn("gb_platform: rtp tcp connect %s:%u failed", ip, port);
            }
        }
        (void)send_ack(sess, m);
        zn_info("gb_platform: INVITE ok channel=%s ssrc=%u media=%s:%u", sess->channel_id,
                 sess->ssrc, ip, port);
    }
}

/*
 * 发起取流 INVITE。
 *
 * 顺序：先开本地 RTP-PS 槽位（拿到 local_port）→ 用它拼 SDP → 建会话并挂链
 * → 最后才发送。会话必须在发送前入链，因为设备可能极快回 200 OK，回调里要靠
 * Call-ID 找到这条会话；若先发后挂会丢响应。发送失败再回滚移除。
 * tcp 参数三态直接映射到 SDP 的传输与 setup（见下方 SDP 构造）。
 */
int zms_gb28181_platform_invite(const char *device_id, const char *channel_id, const char *app,
                                const char *stream, int tcp)
{
    zms_rtp_ps_server_open_opts ropts;
    zms_rtp_ps_server_slot *slot = NULL;
    uint16_t local_port = 0;
    char device_ip[64];
    uint16_t device_port;
    char ssrc_txt[16];
    uint32_t ssrc;
    char from_tag[32];
    char call_id[96];
    char uri[256];
    char from_hdr[256];
    char sdp[1024];
    char buf[4096];
    gb_sip_req_spec spec;
    gb_pf_session *sess;
    const char *app_name;
    const char *stream_name;
    const char *vhost;
    unsigned cseq;
    int n;

    if (!g_pf.running || !device_id || !channel_id) {
        return -1;
    }
    if (tcp < 0 || tcp > 2) {
        return -1;
    }
    zb_mutex_lock(g_pf.mtx);
    {
        gb_pf_device *d = dev_find_locked(device_id);
        if (!d || !d->ip[0]) {
            zb_mutex_unlock(g_pf.mtx);
            return -1;
        }
        copy_str(device_ip, sizeof(device_ip), d->ip);
        device_port = d->port;
    }
    app_name = app && app[0] ? app : (g_pf.media_app[0] ? g_pf.media_app : "gb28181");
    stream_name = stream && stream[0] ? stream : channel_id;
    vhost = g_pf.media_vhost;
    make_ssrc(ssrc_txt, sizeof(ssrc_txt), &ssrc);
    cseq = ++g_pf.cseq;
    zb_mutex_unlock(g_pf.mtx);

    memset(&ropts, 0, sizeof(ropts));
    ropts.poller = g_pf.poller;
    ropts.poller_pool = g_pf.pool;
    ropts.vhost = vhost;
    ropts.app = app_name;
    ropts.stream = stream_name;
    ropts.port = 0;
    ropts.payload_type = ZMS_RTP_PS_DEFAULT_PT;
    ropts.tcp_mode = tcp;
    ropts.ssrc = ssrc;
    ropts.enable_ssrc_filter = 1;
    ropts.signaling_authorized = 1;
    if (zms_rtp_ps_server_open(&ropts, &slot, &local_port) != ZN_OK || !slot) {
        zn_warn("gb_platform: open rtp-ps server failed");
        return -1;
    }

    {
        /* 三态传输的 SDP 差异：
         *   tcp==0 UDP；tcp==1 TCP-PASSIVE（我们监听，设备连入，a=setup:passive）；
         *   tcp==2 TCP-ACTIVE（我们连出到设备，a=setup:active）。
         * 端口用本地槽位实际占用的 local_port，y= 带 ssrc 供设备回填。 */
        char lip[64];
        snprintf(lip, sizeof(lip), "%s", local_ip_for(device_ip));
        snprintf(sdp, sizeof(sdp),
                 "v=0\r\no=%s 0 0 IN IP4 %s\r\ns=Play\r\nc=IN IP4 %s\r\nt=0 0\r\n"
                 "m=video %u %s 96\r\na=recvonly\r\na=rtpmap:96 PS/90000\r\n%s%s"
                 "y=%s\r\n",
                 g_pf.id, lip, lip, (unsigned)local_port,
                 tcp == 0 ? "RTP/AVP" : "TCP/RTP/AVP",
                 tcp == 1 ? "a=setup:passive\r\na=connection:new\r\n"
                          : (tcp == 2 ? "a=setup:active\r\na=connection:new\r\n" : ""),
                 "", ssrc_txt);
    }

    gb_rand_hex(from_tag, 12);
    gb_rand_hex(call_id, 24);
    snprintf(uri, sizeof(uri), "sip:%s@%s:%u", channel_id, device_ip, device_port);
    snprintf(from_hdr, sizeof(from_hdr), "<sip:%s@%s>;tag=%s", g_pf.id, g_pf.domain, from_tag);
    memset(&spec, 0, sizeof(spec));
    spec.method = "INVITE";
    spec.uri = uri;
    spec.from_user = g_pf.id;
    spec.from_host = g_pf.domain;
    spec.from_tag = from_tag;
    spec.to_user = channel_id;
    spec.to_host = g_pf.domain;
    spec.call_id = call_id;
    spec.cseq = cseq;
    spec.local_ip = local_ip_for(device_ip);
    spec.local_port = g_pf.listen_port;
    spec.transport = transport_name();
    spec.contact_user = g_pf.id;
    spec.subject = NULL;
    spec.content_type = "APPLICATION/SDP";
    spec.body = sdp;
    spec.body_len = strlen(sdp);
    {
        static char subject[160];
        snprintf(subject, sizeof(subject), "%s:%s,%s:0", channel_id, ssrc_txt, g_pf.id);
        spec.subject = subject;
    }
    n = gb_sip_build_request(buf, sizeof(buf), &spec);
    if (n <= 0) {
        zms_rtp_ps_server_close(slot);
        return -1;
    }

    sess = (gb_pf_session *)calloc(1, sizeof(*sess));
    if (!sess) {
        zms_rtp_ps_server_close(slot);
        return -1;
    }
    copy_str(sess->call_id, sizeof(sess->call_id), call_id);
    copy_str(sess->device_id, sizeof(sess->device_id), device_id);
    copy_str(sess->channel_id, sizeof(sess->channel_id), channel_id);
    copy_str(sess->vhost, sizeof(sess->vhost), vhost);
    copy_str(sess->app, sizeof(sess->app), app_name);
    copy_str(sess->stream, sizeof(sess->stream), stream_name);
    copy_str(sess->ssrc_txt, sizeof(sess->ssrc_txt), ssrc_txt);
    copy_str(sess->from_hdr, sizeof(sess->from_hdr), from_hdr);
    sess->ssrc = ssrc;
    sess->local_port = local_port;
    sess->tcp = tcp;
    sess->invite_cseq = cseq;
    sess->created_ms = zn_monotonic_ms();
    sess->slot = slot;

    zb_mutex_lock(g_pf.mtx);
    sess->next = g_pf.sessions;
    g_pf.sessions = sess;
    zb_mutex_unlock(g_pf.mtx);

    if (zms_sip_transport_send(g_pf.sip, device_ip, device_port, buf, (size_t)n) != 0) {
        zb_mutex_lock(g_pf.mtx);
        session_remove_locked(call_id);
        zb_mutex_unlock(g_pf.mtx);
        return -1;
    }
    zn_info("gb_platform: INVITE channel=%s ssrc=%s media=%s:%u tcp=%d", channel_id, ssrc_txt,
             vhost[0] ? vhost : "-", local_port, tcp);
    return 0;
}

/* BYE 的 CSeq 必须是 INVITE 的 CSeq+1：SIP 对话框内同一事务序列号递增，
 * 设备端据此把 BYE 关联到对应 INVITE 对话。Contact 优先用 200 OK 里拿到的
 * 对端 URI，没有才退回 channel_id（早期实现/部分设备的兼容）。 */
int zms_gb28181_platform_bye(const char *call_id)
{
    gb_pf_session *sess;
    char buf[4096];
    char branch[32];
    gb_sb w;
    const char *lip;

    if (!g_pf.running || !call_id) {
        return -1;
    }
    zb_mutex_lock(g_pf.mtx);
    sess = session_find_locked(call_id);
    if (!sess) {
        zb_mutex_unlock(g_pf.mtx);
        return -1;
    }
    lip = local_ip_for(sess->device_id);
    gb_rand_hex(branch, 20);
    gb_sb_init(&w, buf, sizeof(buf));
    gb_sb_printf(&w, "BYE %s SIP/2.0\r\n",
                 sess->contact[0] ? sess->contact : sess->channel_id);
    gb_sb_printf(&w, "Via: SIP/2.0/%s %s:%u;rport;branch=z9hG4bK%s\r\n", transport_name(), lip,
                 g_pf.listen_port, branch);
    gb_sb_printf(&w, "From: %s\r\n", sess->from_hdr);
    gb_sb_printf(&w, "To: %s\r\n", sess->to_hdr);
    gb_sb_printf(&w, "Call-ID: %s\r\n", sess->call_id);
    gb_sb_printf(&w, "CSeq: %u BYE\r\n", (unsigned)(sess->invite_cseq + 1));
    gb_sb_puts(&w, "Max-Forwards: 70\r\nContent-Length: 0\r\n\r\n");
    {
        gb_pf_device *d = dev_find_locked(sess->device_id);
        char ip[64];
        uint16_t port;
        if (!d) {
            zb_mutex_unlock(g_pf.mtx);
            return -1;
        }
        copy_str(ip, sizeof(ip), d->ip);
        port = d->port;
        zb_mutex_unlock(g_pf.mtx);
        if (zms_sip_transport_send(g_pf.sip, ip, port, buf, w.len) != 0) {
            return -1;
        }
    }
    return 0;
}

size_t zms_gb28181_platform_sessions(zms_gb28181_platform_session *out, size_t cap)
{
    gb_pf_session *s;
    size_t n = 0;

    if (!out || cap == 0 || !g_pf.mtx) {
        return 0;
    }
    zb_mutex_lock(g_pf.mtx);
    for (s = g_pf.sessions; s && n < cap; s = s->next) {
        zms_gb28181_platform_session *o = &out[n++];
        memset(o, 0, sizeof(*o));
        copy_str(o->call_id, sizeof(o->call_id), s->call_id);
        copy_str(o->device_id, sizeof(o->device_id), s->device_id);
        copy_str(o->channel_id, sizeof(o->channel_id), s->channel_id);
        copy_str(o->vhost, sizeof(o->vhost), s->vhost);
        copy_str(o->app, sizeof(o->app), s->app);
        copy_str(o->stream, sizeof(o->stream), s->stream);
        o->ssrc = s->ssrc;
        o->local_port = s->local_port;
        o->active = s->active;
    }
    zb_mutex_unlock(g_pf.mtx);
    return n;
}

/* ── 生命周期 / API ─────────────────────────────────────────────────────── */

/*
 * 平台启动。进程内单实例：若已在运行先 stop 再按新配置重建。
 * 内部起一个只含 1 个 poller 的线程池承载 SIP 与定时器——GB 平台的信令流量
 * 很小，单 poller 足够且省去跨线程同步；媒体由各自独立的 RTP-PS 槽位处理。
 * UDP 服务端走 server_mode=0（纯监听），TCP/TLS 走 server_mode=1（接受连入）。
 */
int zms_gb28181_platform_start(const zms_gb28181_platform_opts *opts)
{
    zn_poller_pool_opts_t popts;
    zms_sip_transport_opts sopts;
    const char *mode;

    if (!opts || !opts->id || !opts->id[0]) {
        return -1;
    }
    if (g_pf.running) {
        zms_gb28181_platform_stop();
    }
    memset(&g_pf, 0, sizeof(g_pf));
    copy_str(g_pf.id, sizeof(g_pf.id), opts->id);
    copy_str(g_pf.domain, sizeof(g_pf.domain),
             opts->domain && opts->domain[0] ? opts->domain : opts->id);
    copy_str(g_pf.password, sizeof(g_pf.password), opts->password);
    copy_str(g_pf.advertise_ip, sizeof(g_pf.advertise_ip), opts->advertise_ip);
    copy_str(g_pf.tls_cert, sizeof(g_pf.tls_cert), opts->tls_cert);
    copy_str(g_pf.tls_key, sizeof(g_pf.tls_key), opts->tls_key);
    copy_str(g_pf.tls_ca, sizeof(g_pf.tls_ca), opts->tls_ca);
    g_pf.tls_verify_peer = opts->tls_verify_peer;
    copy_str(g_pf.media_vhost, sizeof(g_pf.media_vhost), opts->media_vhost);
    copy_str(g_pf.media_app, sizeof(g_pf.media_app),
             opts->media_app && opts->media_app[0] ? opts->media_app : "gb28181");
    copy_str(g_pf.transport, sizeof(g_pf.transport),
             opts->transport && opts->transport[0] ? opts->transport : "udp");
    g_pf.listen_port = opts->listen_port ? opts->listen_port : 5060;
    g_pf.keepalive_timeout_sec = opts->keepalive_timeout_sec > 0
                                     ? opts->keepalive_timeout_sec
                                     : 180;
    g_pf.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    if (!g_pf.mtx) {
        return -1;
    }

    popts = (zn_poller_pool_opts_t){
        .size = 1,
        .prefer_current_thread = 0,
        .thread_priority = ZB_THREAD_PRIO_NORMAL,
    };
    g_pf.pool = zn_poller_pool_create(&popts);
    if (!g_pf.pool || zn_poller_pool_start(g_pf.pool) != ZN_OK) {
        goto fail;
    }
    g_pf.poller = zn_poller_pool_get_first(g_pf.pool);
    if (!g_pf.poller) {
        goto fail;
    }

    mode = g_pf.transport;
    memset(&sopts, 0, sizeof(sopts));
    sopts.poller = g_pf.poller;
    sopts.poller_pool = g_pf.pool;
    sopts.bind_host = "0.0.0.0";
    sopts.local_port = (uint16_t)g_pf.listen_port;
    sopts.server_mode = 1;
    sopts.tls_cert = g_pf.tls_cert;
    sopts.tls_key = g_pf.tls_key;
    sopts.tls_ca = g_pf.tls_ca;
    sopts.tls_verify_peer = g_pf.tls_verify_peer;
    sopts.on_message = pf_on_message;
    if (strcasecmp(mode, "tcp") == 0) {
        sopts.mode = ZMS_SIP_TRANSPORT_TCP;
    } else if (strcasecmp(mode, "tls") == 0) {
        sopts.mode = ZMS_SIP_TRANSPORT_TLS;
    } else if (strcasecmp(mode, "udp") == 0) {
        sopts.mode = ZMS_SIP_TRANSPORT_UDP;
        sopts.server_mode = 0;
    } else {
        zn_error("gb_platform: unsupported transport=%s", mode);
        goto fail;
    }
    g_pf.sip = zms_sip_transport_create(&sopts);
    if (!g_pf.sip || zms_sip_transport_start(g_pf.sip) != 0) {
        zn_error("gb_platform: SIP %s transport start failed", mode);
        goto fail;
    }
    g_pf.listen_port = zms_sip_transport_local_port(g_pf.sip);
    g_pf.tick = zn_timer_start(g_pf.poller, 1000, 1, pf_tick, NULL);
    if (!g_pf.tick) {
        goto fail;
    }
    g_pf.running = 1;
    zn_info("gb_platform: %s/%s listening %u", g_pf.id, transport_name(), g_pf.listen_port);
    return 0;

fail:
    zms_gb28181_platform_stop();
    return -1;
}

void zms_gb28181_platform_stop(void)
{
    gb_pf_device *d;

    if (g_pf.tick) {
        zn_timer_stop(g_pf.tick);
        g_pf.tick = NULL;
    }
    if (g_pf.sip) {
        zms_sip_transport_destroy(g_pf.sip);
        g_pf.sip = NULL;
    }
    if (g_pf.pool) {
        zn_poller_pool_stop(g_pf.pool);
        zn_poller_pool_destroy(g_pf.pool);
        g_pf.pool = NULL;
        g_pf.poller = NULL;
    }
    if (g_pf.mtx) {
        gb_pf_session *sess;
        zb_mutex_lock(g_pf.mtx);
        d = g_pf.devices;
        g_pf.devices = NULL;
        sess = g_pf.sessions;
        g_pf.sessions = NULL;
        zb_mutex_unlock(g_pf.mtx);
        while (d) {
            gb_pf_device *next = d->next;
            free(d);
            d = next;
        }
        while (sess) {
            gb_pf_session *next = sess->next;
            session_free(sess);
            sess = next;
        }
        zb_mutex_destroy(g_pf.mtx);
        g_pf.mtx = NULL;
    }
    g_pf.running = 0;
}

int zms_gb28181_platform_running(void)
{
    return g_pf.running;
}

size_t zms_gb28181_platform_devices(zms_gb28181_platform_device *out, size_t cap)
{
    gb_pf_device *d;
    size_t n = 0;

    if (!out || cap == 0 || !g_pf.mtx) {
        return 0;
    }
    zb_mutex_lock(g_pf.mtx);
    for (d = g_pf.devices; d && n < cap; d = d->next) {
        zms_gb28181_platform_device *o = &out[n++];
        memset(o, 0, sizeof(*o));
        copy_str(o->id, sizeof(o->id), d->id);
        copy_str(o->ip, sizeof(o->ip), d->ip);
        o->port = d->port;
        o->online = d->online;
        o->last_seen_ms = d->last_seen_ms;
        copy_str(o->name, sizeof(o->name), d->name);
        copy_str(o->manufacturer, sizeof(o->manufacturer), d->manufacturer);
        copy_str(o->model, sizeof(o->model), d->model);
        copy_str(o->firmware, sizeof(o->firmware), d->firmware);
        o->channel_count = d->channel_count;
    }
    zb_mutex_unlock(g_pf.mtx);
    return n;
}

/* 查询通道列表：从设备通道快照拷出（含 device_id 回填），返回条数。
 * 通道数据由 Catalog 应答填充，这里只读不写，持锁短拷贝。 */
size_t zms_gb28181_platform_channels(const char *device_id,
                                     zms_gb28181_platform_channel *out, size_t cap)
{
    gb_pf_device *d;
    size_t n = 0;
    int i;

    if (!device_id || !out || cap == 0 || !g_pf.mtx) {
        return 0;
    }
    zb_mutex_lock(g_pf.mtx);
    d = dev_find_locked(device_id);
    if (d) {
        for (i = 0; i < d->channel_count && n < cap; ++i) {
            zms_gb28181_platform_channel *o = &out[n++];
            memset(o, 0, sizeof(*o));
            copy_str(o->device_id, sizeof(o->device_id), d->id);
            copy_str(o->id, sizeof(o->id), d->channels[i].id);
            copy_str(o->name, sizeof(o->name), d->channels[i].name);
            o->online = d->channels[i].online;
        }
    }
    zb_mutex_unlock(g_pf.mtx);
    return n;
}

/* 对外主动查询入口：只允许 Catalog/DeviceInfo/DeviceStatus 三种，白名单外一律拒绝，
 * 防止把任意字符串当 CmdType 发给设备。 */
int zms_gb28181_platform_query(const char *device_id, const char *cmd)
{
    if (!cmd || (strcmp(cmd, "Catalog") != 0 && strcmp(cmd, "DeviceInfo") != 0 &&
                 strcmp(cmd, "DeviceStatus") != 0)) {
        return -1;
    }
    return send_query(device_id, cmd);
}
