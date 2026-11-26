/**
 * @file session_transport.c
 * @brief 协议 chunk/命令适配到 rtmp_srv_* 会话回调（握手/onStatus/媒体发送）。
 */
#include "session/rtmp/session_transport.h"
#include "session/rtmp/session_internal.h"
#include "zms/media/wire/amf.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RTMP_TYPE_AUDIO 8
#define RTMP_TYPE_VIDEO 9
#define RTMP_TYPE_DATA 18

static void wr_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int zms_rtmp_session_send_media(zms_rtmp_session *s, uint8_t type, const void *body, size_t len,
                                uint32_t tag_dts_ms)
{
    size_t cap;
    uint8_t *buf;
    size_t out_len = 0;
    int rc;

    if (!s || !s->proto || !body || len == 0) {
        return -1;
    }
    cap = len + 4096;
    buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return -1;
    }
    if (zms_rtmp_protocol_send_chunk(s->proto, type, (uint32_t)s->stream_id, tag_dts_ms, body,
                                     len, buf, cap, &out_len) != ZN_OK) {
        free(buf);
        return -1;
    }
    rc = rtmp_srv_send(s, buf, out_len, NULL, 0);
    free(buf);
    return rc < 0 ? -1 : 0;
}

static int send_invoke_ex(zms_rtmp_session *s, const char *cmd, double trans, uint32_t stream_id,
                          const uint8_t *amf, size_t amf_len, int omit_null)
{
    size_t cap;
    uint8_t *buf;
    size_t out_len = 0;
    int rc;

    if (!s || !s->proto || !cmd) {
        return -1;
    }
    cap = amf_len + 4096;
    buf = (uint8_t *)malloc(cap);
    if (!buf) {
        return -1;
    }
    if (zms_rtmp_protocol_send_invoke_ex(s->proto, cmd, trans, stream_id, amf, amf_len, buf, cap,
                                         &out_len, omit_null) != ZN_OK) {
        free(buf);
        return -1;
    }
    rc = rtmp_srv_send(s, buf, out_len, NULL, 0);
    free(buf);
    return rc < 0 ? -1 : 0;
}

static int send_invoke(zms_rtmp_session *s, const char *cmd, double trans, uint32_t stream_id,
                       const uint8_t *amf, size_t amf_len)
{
    return send_invoke_ex(s, cmd, trans, stream_id, amf, amf_len, 0);
}

static int send_control(zms_rtmp_session *s, const uint8_t *body, size_t len)
{
    size_t cap = len + 4096;
    uint8_t *buf = (uint8_t *)malloc(cap);
    size_t out_len = 0;
    int rc;

    if (!s || !s->proto || !buf) {
        return -1;
    }
    if (zms_rtmp_protocol_send_chunk(s->proto, ZMS_RTMP_MSG_USER_CONTROL, 0, 0, body, len, buf,
                                     cap, &out_len) != ZN_OK) {
        free(buf);
        return -1;
    }
    rc = rtmp_srv_send(s, buf, out_len, NULL, 0);
    free(buf);
    return rc < 0 ? -1 : 0;
}

static void send_stream_begin(zms_rtmp_session *s)
{
    uint8_t body[6];
    wr_be16(body, 0); /* StreamBegin */
    wr_be32(body + 2, (uint32_t)s->stream_id);
    (void)send_control(s, body, sizeof(body));
}

static void send_stream_is_recorded(zms_rtmp_session *s)
{
    uint8_t body[6];
    wr_be16(body, 4); /* StreamIsRecorded */
    wr_be32(body + 2, (uint32_t)s->stream_id);
    (void)send_control(s, body, sizeof(body));
}

static void send_status(zms_rtmp_session *s, double trans, const char *level, const char *code,
                        const char *desc)
{
    uint8_t b[512];
    size_t p = 0;

    p += zms_amf_encode_object_begin(b + p, sizeof(b) - p);
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "level");
    p += zms_amf_encode_string(b + p, sizeof(b) - p, level);
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "code");
    p += zms_amf_encode_string(b + p, sizeof(b) - p, code);
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "description");
    p += zms_amf_encode_string(b + p, sizeof(b) - p, desc);
    p += zms_amf_encode_object_end(b + p, sizeof(b) - p);
    (void)send_invoke(s, "onStatus", trans, (uint32_t)s->stream_id, b, p);
}

void zms_rtmp_transport_send_play_start(zms_rtmp_session *s, double transaction, int reset)
{
    if (!s) {
        return;
    }
    send_stream_begin(s);
    if (reset) {
        send_status(s, transaction, "status", "NetStream.Play.Reset", "");
    }
    send_status(s, transaction, "status", "NetStream.Play.Start", "Start video on demand");
    send_stream_is_recorded(s);
}

void zms_rtmp_transport_publish_result(zms_rtmp_session *s, double transaction, int ok)
{
    if (!s) {
        return;
    }
    if (ok) {
        send_stream_begin(s);
        send_status(s, transaction, "status", "NetStream.Publish.Start", "");
    } else {
        /* 鉴权拒绝（对齐 ZLMediaKit 的 NetStream.Publish.BadAuth）。 */
        send_status(s, transaction, "error", "NetStream.Publish.BadAuth", "publish denied");
    }
}

static void send_connect_result(zms_rtmp_session *s, double trans)
{
    uint8_t b[1024];
    size_t p = 0;

    p += zms_amf_encode_object_begin(b + p, sizeof(b) - p);
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "capabilities");
    p += zms_amf_encode_number(b + p, sizeof(b) - p, 31);
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "fmsVer");
    p += zms_amf_encode_string(b + p, sizeof(b) - p, "FMS/3,0,1,123");
    p += zms_amf_encode_object_end(b + p, sizeof(b) - p);

    p += zms_amf_encode_object_begin(b + p, sizeof(b) - p);
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "code");
    p += zms_amf_encode_string(b + p, sizeof(b) - p, "NetConnection.Connect.Success");
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "description");
    p += zms_amf_encode_string(b + p, sizeof(b) - p, "Connection succeeded.");
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "level");
    p += zms_amf_encode_string(b + p, sizeof(b) - p, "status");
    p += zms_amf_encode_object_key(b + p, sizeof(b) - p, "objectEncoding");
    p += zms_amf_encode_null(b + p, sizeof(b) - p);
    p += zms_amf_encode_object_end(b + p, sizeof(b) - p);

    (void)send_invoke_ex(s, "_result", trans, 0, b, p, 1);
}

static void on_connect(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    zms_amf_args *a = zms_amf_args_parse(amf, len);
    const char *app = zms_amf_args_obj_str(a, 0, "app");
    const char *tcurl = zms_amf_args_obj_str(a, 0, "tcUrl");
    const char *vh = NULL;
    char vh_buf[ZMS_VHOST_MAX];

    vh_buf[0] = 0;
    if (app && app[0]) {
        snprintf(s->app, sizeof(s->app), "%s", app);
        {
            char *q = strchr(s->app, '?');

            if (q) {
                char *v = strstr(q + 1, "vhost=");

                /* '?' 之后是客户端查询串（token 等鉴权参数）。 */
                zms_auth_params_copy(q + 1, s->params, sizeof(s->params));
                if (v) {
                    size_t n = 0;

                    v += 6;
                    while (v[n] && v[n] != '&' && n + 1 < sizeof(vh_buf)) {
                        vh_buf[n] = v[n];
                        ++n;
                    }
                    vh_buf[n] = 0;
                    vh = vh_buf;
                }
                *q = 0;
            }
        }
    }
    if (!vh && tcurl && tcurl[0]) {
        vh = tcurl;
    }
    zms_vhost_normalize(vh, s->vhost, sizeof(s->vhost));
    zms_amf_args_free(a);
    zn_info("[rtmp] connect session=%u vhost=%s app=%s", s->session_no, s->vhost, s->app);
    send_connect_result(s, trans);
}

static void on_publish(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    zms_amf_args *a = zms_amf_args_parse(amf, len);
    int r;

    if (!a || !zms_amf_args_str(a, 1)) {
        zms_amf_args_free(a);
        return;
    }
    r = rtmp_srv_onpublish(s, trans, s->app, zms_amf_args_str(a, 1), zms_amf_args_str(a, 2));
    zms_amf_args_free(a);
    zms_rtmp_transport_publish_result(s, trans, r == 0);
}

static void on_play(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    zms_amf_args *a = zms_amf_args_parse(amf, len);
    const char *name;
    double start;
    double duration;
    int reset;
    int r;

    if (!a) {
        return;
    }
    name = zms_amf_args_str(a, 1);
    if (!name) {
        zms_amf_args_free(a);
        return;
    }
    start = zms_amf_args_num(a, 2, -1);
    duration = zms_amf_args_num(a, 3, -1);
    reset = zms_amf_args_bool(a, 4, 0);

    r = rtmp_srv_onplay(s, s->app, name, start, duration, (uint8_t)reset);
    zms_amf_args_free(a);
    if (r < 0) {
        send_status(s, trans, "error", "NetStream.Play.Failed", "play failed");
    } else {
        zms_rtmp_transport_send_play_start(s, trans, reset);
    }
}

static void on_pause(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    zms_amf_args *a = zms_amf_args_parse(amf, len);
    int pause = zms_amf_args_bool(a, 1, 0);
    uint32_t ms = (uint32_t)zms_amf_args_num(a, 2, 0);

    zms_amf_args_free(a);
    (void)rtmp_srv_onpause(s, pause, ms);
    send_status(s, trans, "status", pause ? "NetStream.Pause.Notify" : "NetStream.Unpause.Notify",
                "");
}

static void on_seek(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    zms_amf_args *a = zms_amf_args_parse(amf, len);
    uint32_t ms = (uint32_t)zms_amf_args_num(a, 1, 0);

    zms_amf_args_free(a);
    (void)rtmp_srv_onseek(s, ms);
    send_status(s, trans, "status", "NetStream.Seek.Notify", "");
}

static void on_get_stream_length(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    zms_amf_args *a = zms_amf_args_parse(amf, len);
    const char *name = zms_amf_args_str(a, 1);
    double duration = -1;

    if (name && rtmp_srv_ongetduration(s, s->app, name, &duration) != 0) {
        duration = -1;
    }
    zms_amf_args_free(a);
    {
        uint8_t b[16];
        size_t p = zms_amf_encode_number(b, sizeof(b), duration);

        (void)send_invoke_ex(s, "_result", trans, 0, b, p, 1);
    }
}

static void on_receive_flag(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len,
                            int video)
{
    zms_amf_args *a = zms_amf_args_parse(amf, len);
    int on = zms_amf_args_bool(a, 1, 1);

    zms_amf_args_free(a);
    if (video) {
        s->receive_video = on;
    } else {
        s->receive_audio = on;
    }
    if (on) {
        send_status(s, trans, "status", "NetStream.Seek.Notify", "");
        send_status(s, trans, "status", "NetStream.Play.Start", "");
    }
}

static void on_create_stream(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    uint8_t b[16];
    size_t p;

    (void)amf;
    (void)len;
    s->stream_id = 1;
    p = zms_amf_encode_number(b, sizeof(b), 1);
    (void)send_invoke(s, "_result", trans, 0, b, p);
}

/* releaseStream / FCPublish / FCUnpublish / closeStream：回 _result(null)。 */
static void on_simple_null(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    uint8_t null_v[1];
    size_t p = zms_amf_encode_null(null_v, sizeof(null_v));

    (void)amf;
    (void)len;
    (void)send_invoke(s, "_result", trans, 0, null_v, p);
}

static void on_delete_stream(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    (void)amf;
    (void)len;
    send_status(s, trans, "status", "NetStream.DeleteStream.Suceess", "");
}

static void on_receive_audio(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    on_receive_flag(s, trans, amf, len, 0);
}

static void on_receive_video(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len)
{
    on_receive_flag(s, trans, amf, len, 1);
}

typedef void (*rtmp_cmd_fn)(zms_rtmp_session *s, double trans, const uint8_t *amf, size_t len);

/* 命令 → 处理函数。新增命令只加一行，不再拉长 if/else 链。 */
static const struct {
    const char *name;
    rtmp_cmd_fn fn;
} k_rtmp_cmds[] = {
    {"connect", on_connect},
    {"createStream", on_create_stream},
    {"releaseStream", on_simple_null},
    {"FCPublish", on_simple_null},
    {"FCUnpublish", on_simple_null},
    {"closeStream", on_simple_null},
    {"deleteStream", on_delete_stream},
    {"publish", on_publish},
    {"play", on_play},
    {"pause", on_pause},
    {"seek", on_seek},
    {"getStreamLength", on_get_stream_length},
    {"receiveAudio", on_receive_audio},
    {"receiveVideo", on_receive_video},
};

static void transport_on_cmd(const char *cmd, double trans_id, const uint8_t *amf_rest,
                             size_t amf_rest_len, void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;

    if (!s || !cmd || s->destroy_scheduled) {
        return;
    }
    for (size_t i = 0; i < sizeof(k_rtmp_cmds) / sizeof(k_rtmp_cmds[0]); ++i) {
        if (strcmp(cmd, k_rtmp_cmds[i].name) == 0) {
            k_rtmp_cmds[i].fn(s, trans_id, amf_rest, amf_rest_len);
            return;
        }
    }
}

static void transport_on_chunk(const zms_rtmp_chunk *chunk, void *user)
{
    zms_rtmp_session *s = (zms_rtmp_session *)user;

    if (!s || !chunk || s->destroy_scheduled) {
        return;
    }
    switch (chunk->type_id) {
    case RTMP_TYPE_AUDIO:
        if (s->receive_audio) {
            (void)rtmp_srv_onaudio(s, chunk->body, chunk->body_size, chunk->tag_dts_ms);
        }
        break;
    case RTMP_TYPE_VIDEO:
        if (s->receive_video) {
            (void)rtmp_srv_onvideo(s, chunk->body, chunk->body_size, chunk->tag_dts_ms);
        }
        break;
    case RTMP_TYPE_DATA:
        (void)rtmp_srv_onscript(s, chunk->body, chunk->body_size, chunk->tag_dts_ms);
        break;
    case ZMS_RTMP_MSG_USER_CONTROL:
        if (chunk->body_size >= 6 && chunk->body[0] == 0 && chunk->body[1] == 6) {
            uint8_t pong[6];
            pong[0] = 0;
            pong[1] = 7; /* PingResponse */
            memcpy(pong + 2, chunk->body + 2, 4);
            (void)send_control(s, pong, sizeof(pong));
        }
        break;
    default:
        break;
    }
}

zms_rtmp_protocol *zms_rtmp_transport_create(zms_rtmp_session *s)
{
    zms_rtmp_protocol_opts opts;

    if (!s) {
        return NULL;
    }
    memset(&opts, 0, sizeof(opts));
    opts.on_chunk = transport_on_chunk;
    opts.on_cmd = transport_on_cmd;
    opts.user = s;
    return zms_rtmp_protocol_create(&opts);
}

zn_err_t zms_rtmp_transport_input(zms_rtmp_session *s, const void *data, size_t len)
{
    if (!s || !s->proto || !data) {
        return ZN_ERR_INVALID;
    }
    return zms_rtmp_protocol_input(s->proto, data, len);
}
