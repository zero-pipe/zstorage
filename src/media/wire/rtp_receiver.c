/**
 * @file rtp_receiver.c
 * @brief RTP 接收（jitter/reorder 队列）。
 */
#include "zcs/media/wire/rtp_receiver.h"
#include "zcs/media/media_limits.h"
#include "zm/transport/rtp.h"
#include <stdlib.h>
#include <string.h>

#define RTP_CLOCK_90KHZ 90000

struct zcs_rtp_receiver {
    unsigned max_track;
    int jitter_ms;
    zcs_rtp_sorted_cb on_sorted;
    void *user;
    rtp_queue_t **queues;
};

static struct rtp_packet_t *rtp_pkt_alloc(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > 64 * 1024) {
        return NULL;
    }

    uint8_t *buf = (uint8_t *)malloc(sizeof(int) + sizeof(struct rtp_packet_t) + len);
    if (!buf) {
        return NULL;
    }

    *(int *)buf = (int)len;
    struct rtp_packet_t *pkt = (struct rtp_packet_t *)(buf + sizeof(int));
    memcpy(pkt + 1, data, len);
    if (rtp_packet_deserialize(pkt, pkt + 1, (int)len) != 0) {
        free(buf);
        return NULL;
    }
    return pkt;
}

static void rtp_pkt_free(void *param, struct rtp_packet_t *pkt)
{
    (void)param;
    if (!pkt) {
        return;
    }
    free((uint8_t *)pkt - sizeof(int));
}

static void drain_track(zcs_rtp_receiver *r, int track_index)
{
    rtp_queue_t *q = r->queues[track_index];
    struct rtp_packet_t *pkt;
    while ((pkt = rtp_queue_read(q)) != NULL) {
        int bytes = *(int *)((uint8_t *)pkt - sizeof(int));
        const uint8_t *raw = (const uint8_t *)(pkt + 1);

        zcs_rtp_packet out;
        if (zcs_rtp_parse(raw, (size_t)bytes, &out) == ZN_OK && r->on_sorted) {
            r->on_sorted(&out, track_index, r->user);
        }

        rtp_pkt_free(NULL, pkt);
    }
}

zcs_rtp_receiver *zcs_rtp_receiver_create(const zcs_rtp_receiver_opts *opts)
{
    if (!opts || opts->max_track == 0) {
        return NULL;
    }

    zcs_rtp_receiver *r = (zcs_rtp_receiver *)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }

    int jitter_ms = opts->jitter_ms > 0 ? opts->jitter_ms : ZCS_RTP_JITTER_MS_UDP_DEFAULT;

    r->max_track = opts->max_track;
    r->jitter_ms = jitter_ms;
    r->on_sorted = opts->on_sorted;
    r->user = opts->user;

    r->queues = (rtp_queue_t **)calloc(opts->max_track, sizeof(rtp_queue_t *));
    if (!r->queues) {
        free(r);
        return NULL;
    }

    for (unsigned i = 0; i < opts->max_track; ++i) {
        r->queues[i] = rtp_queue_create(r->jitter_ms, RTP_CLOCK_90KHZ, rtp_pkt_free, NULL);
        if (!r->queues[i]) {
            zcs_rtp_receiver_destroy(r);
            return NULL;
        }
    }
    return r;
}

void zcs_rtp_receiver_destroy(zcs_rtp_receiver *r)
{
    if (!r) {
        return;
    }
    if (r->queues) {
        for (unsigned i = 0; i < r->max_track; ++i) {
            if (r->queues[i]) {
                rtp_queue_destroy(r->queues[i]);
            }
        }
        free(r->queues);
    }
    free(r);
}

zn_err_t zcs_rtp_receiver_input(zcs_rtp_receiver *r, int track_index, const uint8_t *data,
                                 size_t len)
{
    if (!r || track_index < 0 || (unsigned)track_index >= r->max_track || !data || len == 0) {
        return ZN_ERR_INVALID;
    }

    struct rtp_packet_t *pkt = rtp_pkt_alloc(data, len);
    if (!pkt) {
        return ZN_ERR_INVALID;
    }

    int wr = rtp_queue_write(r->queues[track_index], pkt);
    if (wr <= 0) {
        rtp_pkt_free(NULL, pkt);
        return wr < 0 ? ZN_ERR_INVALID : ZN_ERR_AGAIN;
    }

    drain_track(r, track_index);
    return ZN_OK;
}

void zcs_rtp_receiver_flush(zcs_rtp_receiver *r)
{
    if (!r) {
        return;
    }
    for (unsigned i = 0; i < r->max_track; ++i) {
        if (r->queues[i]) {
            rtp_queue_destroy(r->queues[i]);
        }
        r->queues[i] = rtp_queue_create(r->jitter_ms, RTP_CLOCK_90KHZ, rtp_pkt_free, NULL);
    }
}

void zcs_rtp_receiver_reset(zcs_rtp_receiver *r)
{
    zcs_rtp_receiver_flush(r);
}
