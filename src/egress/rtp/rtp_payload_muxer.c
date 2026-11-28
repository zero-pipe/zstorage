/**
 * @file rtp_payload_muxer.c
 * @brief RTSP RTP 输出：把编码帧按 RTP 负载打包。
 *
 * 只负责 RTP payload 打包；时间线由宿主 (play clock) 通过
 * rtp_payload_muxer_sync_timeline 提供，SSRC/seq/RTCP 调度留在 ZCS 会话层。
 * 打包器本身由 ZM 的 zm_rtp_writer 提供。
 */
#include "egress/rtp/rtp_payload_muxer.h"

#include "zm/transport/rtp.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct rtp_muxer_media_t;
struct rtp_muxer_payload_t;

struct rtp_muxer_payload_t {
    struct rtp_payload_muxer_t *muxer;
    zm_writer *rtp;

    int frequency; /* RTP clock rate */
    uint32_t timeline_rtp;
    int64_t timeline_pts;

    int pid;
    int port;

    uint16_t seq;       /* 下一个包的 seq（由已发包 RTP 头推导） */
    uint32_t timestamp; /* 最近一次发出的 RTP 时间戳 */
};

struct rtp_muxer_media_t {
    int codec;
    struct rtp_muxer_payload_t *pt;
};

struct rtp_payload_muxer_t {
    struct rtp_muxer_payload_t payloads[3];
    int payload_capacity;
    int payload_count;

    struct rtp_muxer_media_t medias[4];
    int media_capacity;
    int media_count;

    rtp_payload_muxer_onpacket onpacket;
    void *param;
};

static uint32_t rtp_payload_muxer_timeline_update(struct rtp_muxer_payload_t *pt, int64_t pts)
{
    if (0 != pt->timeline_pts)
        pt->timeline_rtp += (int32_t)(pts - pt->timeline_pts);
    pt->timeline_pts = pts;
    return pt->timeline_rtp;
}

static int rtp_payload_muxer_sink_write(void *param, const void *packet, size_t bytes)
{
    struct rtp_muxer_payload_t *pt = (struct rtp_muxer_payload_t *)param;
    const uint8_t *rtp = (const uint8_t *)packet;
    uint32_t timestamp;

    if (!pt || !packet || bytes < 12) {
        return -1;
    }
    timestamp = ((uint32_t)rtp[4] << 24) | ((uint32_t)rtp[5] << 16) | ((uint32_t)rtp[6] << 8) |
                (uint32_t)rtp[7];
    pt->seq = (uint16_t)((((uint16_t)rtp[2] << 8) | rtp[3]) + 1u);
    pt->timestamp = timestamp;
    return pt->muxer->onpacket(pt->muxer->param, pt->pid, packet, (int)bytes, timestamp, 0);
}

struct rtp_payload_muxer_t *rtp_payload_muxer_create(rtp_payload_muxer_onpacket onpacket, void *param)
{
    struct rtp_payload_muxer_t *muxer = calloc(1, sizeof(*muxer));

    if (!muxer)
        return NULL;

    muxer->payload_capacity = (int)(sizeof(muxer->payloads) / sizeof(muxer->payloads[0]));
    muxer->media_capacity = (int)(sizeof(muxer->medias) / sizeof(muxer->medias[0]));
    muxer->onpacket = onpacket;
    muxer->param = param;
    return muxer;
}

int rtp_payload_muxer_destroy(struct rtp_payload_muxer_t *muxer)
{
    int i;

    if (muxer) {
        for (i = 0; i < muxer->payload_count; i++) {
            struct rtp_muxer_payload_t *pt = &muxer->payloads[i];
            if (pt->rtp) {
                zm_writer_close(pt->rtp);
                pt->rtp = NULL;
            }
        }
        free(muxer);
    }
    return 0;
}

int rtp_payload_muxer_add_payload(struct rtp_payload_muxer_t *muxer, const char *proto, int frequence,
                           int payload, const char *encoding, uint16_t seq, uint32_t ssrc,
                           uint16_t port, const void *extra, int size)
{
    struct rtp_muxer_payload_t *pt;
    zm_rtp_writer_cfg cfg;
    zm_sink sink;

    (void)proto;
    (void)extra;
    (void)size;

    if (!muxer || !encoding)
        return -EINVAL;
    if (muxer->payload_count >= muxer->payload_capacity)
        return -E2BIG;

    pt = &muxer->payloads[muxer->payload_count];
    memset(pt, 0, sizeof(*pt));
    pt->muxer = muxer;
    pt->pid = muxer->payload_count;
    pt->port = port;
    pt->frequency = frequence > 0 ? frequence : 90000;
    pt->seq = seq;

    memset(&cfg, 0, sizeof(cfg));
    cfg.encoding = encoding;
    cfg.payload_type = payload;
    cfg.clock_hz = pt->frequency;
    cfg.seq = seq;
    cfg.ssrc = ssrc;

    memset(&sink, 0, sizeof(sink));
    sink.ctx = pt;
    sink.write = rtp_payload_muxer_sink_write;

    pt->rtp = zm_rtp_writer_open(&sink, &cfg);
    if (!pt->rtp)
        return -EPROTONOSUPPORT;

    muxer->payload_count++;
    return pt->pid;
}

int rtp_payload_muxer_add_media(struct rtp_payload_muxer_t *muxer, int pid, int codec, const void *extra,
                         int size)
{
    struct rtp_muxer_media_t *m;

    (void)extra;
    (void)size;

    if (!muxer)
        return -EINVAL;
    if (muxer->media_count >= muxer->media_capacity)
        return -E2BIG;
    if (pid < 0 || pid >= muxer->payload_count)
        return -ENOENT;

    m = &muxer->medias[muxer->media_count];
    memset(m, 0, sizeof(*m));
    m->pt = &muxer->payloads[pid];
    m->codec = codec;
    return muxer->media_count++;
}

int rtp_payload_muxer_getinfo(struct rtp_payload_muxer_t *muxer, int pid, uint16_t *seq, uint32_t *timestamp,
                       const char **sdp, int *size)
{
    struct rtp_muxer_payload_t *pt;

    if (!muxer || pid < 0 || pid >= muxer->payload_count)
        return -ENOENT;

    pt = &muxer->payloads[pid];
    if (seq)
        *seq = pt->seq;
    if (timestamp)
        *timestamp = pt->timestamp;
    if (sdp)
        *sdp = NULL;
    if (size)
        *size = 0;
    return 0;
}

int rtp_payload_muxer_input(struct rtp_payload_muxer_t *muxer, int mid, int64_t pts, int64_t dts,
                     const void *data, int bytes, int flags)
{
    struct rtp_muxer_media_t *m;
    uint32_t ts;

    (void)dts;
    (void)flags;

    if (!muxer || mid < 0 || mid >= muxer->media_count)
        return -ENOENT;

    m = &muxer->medias[mid];
    ts = rtp_payload_muxer_timeline_update(m->pt, pts * m->pt->frequency / 1000);
    return zm_rtp_writer_write_ts(m->pt->rtp, data, (size_t)bytes, ts) == ZM_OK ? 0 : -1;
}

int rtp_payload_muxer_sync_timeline(struct rtp_payload_muxer_t *muxer, int pid, int64_t dts_ms, uint32_t rtp_ts)
{
    struct rtp_muxer_payload_t *pt;

    if (!muxer || pid < 0 || pid >= muxer->payload_count)
        return -ENOENT;
    pt = &muxer->payloads[pid];
    if (dts_ms < 0)
        dts_ms = 0;
    pt->timeline_rtp = rtp_ts;
    pt->timeline_pts = dts_ms * pt->frequency / 1000;
    return 0;
}
