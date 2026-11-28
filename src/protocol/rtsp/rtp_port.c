/**
 * @file rtp_port.c
 * @brief RTP over UDP 端口池（分配/回收）。
 */
#include "protocol/rtsp/rtp_port.h"
#include <stdlib.h>
#include <string.h>

#define ZN_RTP_PORT_DEFAULT_START 30000u
#define ZN_RTP_PORT_DEFAULT_END   39998u

struct zcs_rtp_port_pool {
    uint16_t range_start;
    uint16_t range_end;
    uint16_t *free_ports;
    unsigned free_cap;
    unsigned free_count;
};

static int port_valid(uint16_t p, uint16_t start, uint16_t end)
{
    return (p >= start && p <= end && (p & 1u) == 0);
}

static int pool_push(zcs_rtp_port_pool *pool, uint16_t port)
{
    if (pool->free_count >= pool->free_cap) {
        unsigned ncap = pool->free_cap ? pool->free_cap * 2u : 64u;
        uint16_t *nbuf = (uint16_t *)realloc(pool->free_ports, ncap * sizeof(uint16_t));
        if (!nbuf)
            return 0;
        pool->free_ports = nbuf;
        pool->free_cap = ncap;
    }
    pool->free_ports[pool->free_count++] = port;
    return 1;
}

static int pool_pop(zcs_rtp_port_pool *pool, uint16_t *port)
{
    if (pool->free_count == 0)
        return 0;
    *port = pool->free_ports[--pool->free_count];
    return 1;
}

zcs_rtp_port_pool *zcs_rtp_port_pool_create(const zcs_rtp_port_pool_opts *opts)
{
    uint16_t start = ZN_RTP_PORT_DEFAULT_START;
    uint16_t end = ZN_RTP_PORT_DEFAULT_END;

    if (opts) {
        if (opts->range_start)
            start = opts->range_start;
        if (opts->range_end)
            end = opts->range_end;
    }
    if ((start & 1u) || (end & 1u) || start > end)
        return NULL;

    zcs_rtp_port_pool *pool = (zcs_rtp_port_pool *)calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->range_start = start;
    pool->range_end = end;

    for (uint32_t p = start; p <= end; p += 2u) {
        if (!pool_push(pool, (uint16_t)p)) {
            zcs_rtp_port_pool_destroy(pool);
            return NULL;
        }
    }
    return pool;
}

void zcs_rtp_port_pool_destroy(zcs_rtp_port_pool *pool)
{
    if (!pool)
        return;
    free(pool->free_ports);
    free(pool);
}

zn_err_t zcs_rtp_port_pool_acquire(zcs_rtp_port_pool *pool, uint16_t *rtp_port, uint16_t *rtcp_port)
{
    if (!pool || !rtp_port || !rtcp_port)
        return ZN_ERR_INVALID;

    uint16_t rtp;
    if (!pool_pop(pool, &rtp))
        return ZN_ERR_NOMEM;

    *rtp_port = rtp;
    *rtcp_port = (uint16_t)(rtp + 1u);
    return ZN_OK;
}

void zcs_rtp_port_pool_release(zcs_rtp_port_pool *pool, uint16_t rtp_port)
{
    if (!pool || !port_valid(rtp_port, pool->range_start, pool->range_end))
        return;
    (void)pool_push(pool, rtp_port);
}

unsigned zcs_rtp_port_pool_free_count(const zcs_rtp_port_pool *pool)
{
    return pool ? pool->free_count : 0;
}
