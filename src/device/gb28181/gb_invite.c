/**
 * @file gb_invite.c
 * @brief GB28181 INVITE/ACK/BYE/INFO：SDP 协商与点播/回放会话建立。
 */
#include "device/gb28181/gb28181_internal.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) && defined(_MSC_VER)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

typedef struct gb_sdp_offer {
    char media_ip[64];
    unsigned media_port;
    char proto[32];
    char setup[16];
    uint8_t pt;
    char ssrc[32];
    char session[32]; /**< s=Play / Playback / Download */
    char f_param[128];
    int has_time;
    time_t start_time;
    time_t end_time;
} gb_sdp_offer;

static int parse_ssrc_from_subject(const char *subject, char *out, size_t cap)
{
    const char *p;
    const char *colon;
    const char *end;

    out[0] = '\0';
    if (!subject) {
        return -1;
    }
    colon = strchr(subject, ':');
    if (!colon) {
        return -1;
    }
    p = colon + 1;
    end = strchr(p, ',');
    if (!end) {
        end = p + strlen(p);
    }
    while (p < end && (*p == ' ' || *p == '\t')) {
        ++p;
    }
    while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
        --end;
    }
    if (end <= p || (size_t)(end - p) >= cap) {
        return -1;
    }
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return 0;
}

static uint32_t ssrc_from_str(const char *s)
{
    uint32_t v = 0;

    while (s && *s >= '0' && *s <= '9') {
        v = v * 10u + (uint32_t)(*s - '0');
        ++s;
    }
    return v ? v : 0x0A0B0C0Du;
}

static void parse_sdp(const char *body, gb_sdp_offer *o)
{
    char line[512];
    const char *p = body;
    uint8_t rtpmap_pt = 0;

    memset(o, 0, sizeof(*o));
    o->pt = 96;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t n;
        char *s;

        if (!eol) {
            eol = p + strlen(p);
        }
        n = (size_t)(eol - p);
        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p = *eol ? eol + 2 : eol;
        s = line;
        while (*s == ' ' || *s == '\t') {
            ++s;
        }
        if (strncmp(s, "s=", 2) == 0) {
            snprintf(o->session, sizeof(o->session), "%s", s + 2);
        } else if (strncmp(s, "c=IN IP4 ", 9) == 0) {
            snprintf(o->media_ip, sizeof(o->media_ip), "%s", s + 9);
        } else if (strncmp(s, "t=", 2) == 0) {
            long long a = 0, b = 0;
            if (sscanf(s + 2, "%lld %lld", &a, &b) == 2 && a > 0) {
                o->has_time = 1;
                o->start_time = (time_t)a;
                o->end_time = (time_t)b;
            }
        } else if (strncmp(s, "m=video ", 8) == 0) {
            unsigned port = 0;
            char proto[32] = "";
            int pt = 0;
            if (sscanf(s + 8, "%u %31s %d", &port, proto, &pt) >= 2) {
                o->media_port = port;
                snprintf(o->proto, sizeof(o->proto), "%s", proto);
                if (pt > 0 && pt < 128) {
                    o->pt = (uint8_t)pt;
                }
            }
        } else if (strncmp(s, "a=setup:", 8) == 0) {
            snprintf(o->setup, sizeof(o->setup), "%s", s + 8);
        } else if (strncmp(s, "a=rtpmap:", 9) == 0) {
            int pt = 0;
            char enc[32] = "";
            if (sscanf(s + 9, "%d %31[^/]", &pt, enc) == 2 && strcasecmp(enc, "PS") == 0) {
                rtpmap_pt = (uint8_t)pt;
            }
        } else if (strncmp(s, "y=", 2) == 0) {
            snprintf(o->ssrc, sizeof(o->ssrc), "%s", s + 2);
        } else if (strncmp(s, "f=", 2) == 0) {
            snprintf(o->f_param, sizeof(o->f_param), "%s", s + 2);
        }
    }
    if (rtpmap_pt) {
        o->pt = rtpmap_pt;
    }
    if (!o->media_ip[0]) {
        o->media_ip[0] = '\0';
    }
}

static int build_answer_sdp(char *out, size_t cap, const gb_sdp_offer *o, uint16_t local_port,
                            uint32_t ssrc, gb_media_transport transport, int is_playback)
{
    gb_sb w;
    const char *proto = transport == GB_MEDIA_UDP ? "RTP/AVP" : "TCP/RTP/AVP";

    gb_sb_init(&w, out, cap);
    gb_sb_puts(&w, "v=0\r\n");
    gb_sb_printf(&w, "o=%s 0 0 IN IP4 %s\r\n", gb_ctx()->device_id, gb_ctx()->local_ip);
    gb_sb_printf(&w, "s=%s\r\n", is_playback ? "Playback" : "Play");
    gb_sb_printf(&w, "c=IN IP4 %s\r\n", gb_ctx()->local_ip);
    if (is_playback && o->has_time) {
        gb_sb_printf(&w, "t=%lld %lld\r\n", (long long)o->start_time,
                     (long long)o->end_time);
    } else {
        gb_sb_puts(&w, "t=0 0\r\n");
    }
    gb_sb_printf(&w, "m=video %u %s %u\r\n", (unsigned)local_port, proto, (unsigned)o->pt);
    gb_sb_puts(&w, "a=sendonly\r\n");
    gb_sb_printf(&w, "a=rtpmap:%u PS/90000\r\n", (unsigned)o->pt);
    if (transport != GB_MEDIA_UDP) {
        gb_sb_printf(&w, "a=setup:%s\r\n", transport == GB_MEDIA_TCP_PASSIVE ? "passive" : "active");
        gb_sb_puts(&w, "a=connection:new\r\n");
    }
    gb_sb_printf(&w, "y=%u\r\n", ssrc);
    if (o->f_param[0]) {
        gb_sb_printf(&w, "f=%s\r\n", o->f_param);
    }
    return (int)w.len;
}

static gb_media_transport pick_transport(const gb_sdp_offer *o)
{
    if (o->proto[0] && strstr(o->proto, "TCP") != NULL) {
        if (strcasecmp(o->setup, "passive") == 0) {
            /* 平台 listen，本端主动连 */
            return GB_MEDIA_TCP_ACTIVE;
        }
        /* setup:active 或未声明：平台连入，本端 listen */
        return GB_MEDIA_TCP_PASSIVE;
    }
    return GB_MEDIA_UDP;
}

static int is_playback_offer(const gb_sdp_offer *o, const gb_sip_msg *m)
{
    if (strcasecmp(o->session, "Playback") == 0 || strcasecmp(o->session, "Download") == 0) {
        return 1;
    }
    {
        const char *u = gb_sip_hdr(m, "Subject");
        (void)u;
    }
    return 0;
}

void gb_invite_handle(const gb_sip_msg *m)
{
    char channel_id[ZMS_GB28181_ID_MAX];
    char platform_id[ZMS_GB28181_ID_MAX];
    char to_tag[64];
    char sdp[1024];
    char answer[1024];
    char subject_ssrc[32];
    const char *subject;
    const char *contact;
    gb_sdp_offer offer;
    gb_channel ch;
    gb_media_session *sess;
    gb_media_transport transport;
    uint32_t ssrc;
    int local_port = 0;
    int is_playback;

    gb_sip_uri_user(gb_sip_hdr(m, "To"), channel_id, sizeof(channel_id));
    gb_sip_uri_user(gb_sip_hdr(m, "From"), platform_id, sizeof(platform_id));
    if (!channel_id[0]) {
        gb_send_response(m, 400, "Bad Request", NULL, NULL, NULL, NULL, 0);
        return;
    }
    if (gb_channel_get(channel_id, &ch) != 0) {
        zn_warn("gb28181: INVITE unknown channel %s", channel_id);
        gb_send_response(m, 404, "Not Found", NULL, NULL, NULL, NULL, 0);
        return;
    }

    /* 解析 SDP（body 已 NUL 结尾） */
    parse_sdp(m->body, &offer);
    if (!offer.media_ip[0]) {
        snprintf(offer.media_ip, sizeof(offer.media_ip), "%s", m->peer_ip);
    }
    if (!offer.media_port) {
        offer.media_port = m->peer_port; /* TCP 主动模式平台媒体端口由 m= 给出 */
    }
    subject = gb_sip_hdr(m, "Subject");
    if (parse_ssrc_from_subject(subject, subject_ssrc, sizeof(subject_ssrc)) == 0) {
        snprintf(offer.ssrc, sizeof(offer.ssrc), "%s", subject_ssrc);
    }
    ssrc = ssrc_from_str(offer.ssrc);

    transport = pick_transport(&offer);
    is_playback = is_playback_offer(&offer, m);

    if (!is_playback && !gb_channel_source(&ch)) {
        zn_warn("gb28181: INVITE channel=%s but stream %s/%s/%s is offline", channel_id,
                 ch.vhost, ch.app, ch.stream);
        gb_send_response(m, 404, "Not Found", NULL, NULL, NULL, NULL, 0);
        return;
    }
    if (is_playback) {
        gb_record_list *list;
        int have = 0;
        char st[40];
        char en[40];

        if (offer.has_time) {
            gb_format_time_iso(offer.start_time, st, sizeof(st));
            gb_format_time_iso(offer.end_time, en, sizeof(en));
        } else {
            st[0] = en[0] = '\0';
        }
        list = gb_record_query(channel_id, st, en);
        if (list) {
            have = zms_device_record_list_count(list) > 0;
            zms_device_record_list_free(list);
        }
        if (!have) {
            zn_warn("gb28181: playback INVITE channel=%s but no record in range", channel_id);
            gb_send_response(m, 404, "Not Found", NULL, NULL, NULL, NULL, 0);
            return;
        }
    }

    sess = gb_media_open(gb_sip_hdr(m, "Call-ID"), channel_id, platform_id, ssrc, offer.pt,
                         transport, offer.media_ip, (uint16_t)offer.media_port, &local_port);
    if (!sess) {
        zn_error("gb28181: media open failed channel=%s", channel_id);
        gb_send_response(m, 500, "Server Error", NULL, NULL, NULL, NULL, 0);
        return;
    }
    sess->is_playback = is_playback;
    if (is_playback && offer.has_time) {
        gb_format_time_iso(offer.start_time, sess->play_start, sizeof(sess->play_start));
        gb_format_time_iso(offer.end_time, sess->play_end, sizeof(sess->play_end));
    }

    (void)build_answer_sdp(answer, sizeof(answer), &offer, (uint16_t)local_port, ssrc, transport, is_playback);
    (void)sdp;

    gb_rand_hex(to_tag, 12);
    contact = gb_ctx()->device_id;
    zn_info("gb28181: INVITE channel=%s ssrc=%u pt=%u transport=%d local=%u remote=%s:%u %s",
             channel_id, ssrc, offer.pt, (int)transport, local_port, offer.media_ip,
             offer.media_port, is_playback ? "playback" : "play");
    gb_send_response(m, 200, "OK", to_tag, contact, "APPLICATION/SDP", answer, strlen(answer));
}

void gb_invite_handle_ack(const gb_sip_msg *m)
{
    const char *call_id = gb_sip_hdr(m, "Call-ID");
    gb_media_session *sess = gb_media_find_call(call_id);
    char channel_id[ZMS_GB28181_ID_MAX];

    if (!sess) {
        return;
    }
    gb_sip_uri_user(gb_sip_hdr(m, "To"), channel_id, sizeof(channel_id));
    {
        gb_channel ch;
        const char *cid = channel_id[0] ? channel_id : sess->channel_id;
        int found;

        memset(&ch, 0, sizeof(ch));
        found = gb_channel_get(cid, &ch) == 0;
        if (!found && strcmp(cid, sess->channel_id) != 0) {
            found = gb_channel_get(sess->channel_id, &ch) == 0;
        }
        if (found && !sess->started) {
            if (gb_media_start(sess, &ch) == 0) {
                zn_info("gb28181: media started channel=%s call=%s", ch.id, sess->call_id);
            } else {
                zn_warn("gb28181: media start failed channel=%s", ch.id);
            }
        }
    }
    gb_media_session_unref(sess);
}

void gb_invite_handle_bye(const gb_sip_msg *m)
{
    const char *call_id = gb_sip_hdr(m, "Call-ID");

    if (call_id && gb_media_stop_call(call_id)) {
        zn_info("gb28181: BYE call=%s", call_id);
    }
    gb_send_response(m, 200, "OK", NULL, NULL, NULL, NULL, 0);
}

void gb_invite_handle_info(const gb_sip_msg *m)
{
    /* MANSRTSP：暂停/继续/倍速/seek。首期只应答，回放线程按实时速度播放。 */
    if (m->body_len) {
        zn_info("gb28181: MANSRTSP INFO: %.64s", m->body);
    }
    gb_send_response(m, 200, "OK", NULL, NULL, NULL, NULL, 0);
}
