/**
 * @file dns_cache.c
 * @brief 主机名解析缓存实现。
 */
#include "zms/util/dns_cache.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#define DNS_CACHE_SLOTS 64
#define DNS_HOST_MAX 256

typedef struct dns_entry {
    char host[DNS_HOST_MAX];
    char ip[64];
    int used;
} dns_entry;

static dns_entry g_entries[DNS_CACHE_SLOTS];
static zb_mutex *g_mutex;
static int g_next_slot;

static void cache_lock(void)
{
    if (!g_mutex) {
        g_mutex = zb_mutex_create(ZB_MUTEX_NORMAL);
    }
    if (g_mutex) {
        zb_mutex_lock(g_mutex);
    }
}

static void cache_unlock(void)
{
    if (g_mutex) {
        zb_mutex_unlock(g_mutex);
    }
}

/* 主机名是否已经是 IP 文本。 */
static int is_ip_literal(const char *host)
{
    unsigned char buf[16];

    return inet_pton(AF_INET, host, buf) == 1 || inet_pton(AF_INET6, host, buf) == 1;
}

static int resolve_uncached(const char *host, char *out, size_t cap)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    const char *picked = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        if (out && cap) {
            out[0] = '\0';
        }
        return -1;
    }
    /* 优先 IPv4，找不到再退 IPv6。 */
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            picked = inet_ntop(AF_INET, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, out,
                               (socklen_t)cap);
            break;
        }
    }
    if (!picked) {
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET6) {
                picked = inet_ntop(AF_INET6, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, out,
                                   (socklen_t)cap);
                break;
            }
        }
    }
    freeaddrinfo(res);
    if (!picked && out && cap) {
        out[0] = '\0';
    }
    return picked ? 0 : -1;
}

int zms_dns_resolve(const char *host, char *out_ip, size_t cap)
{
    char ip[64];

    if (!host || !host[0] || !out_ip || cap == 0) {
        return -1;
    }
    out_ip[0] = '\0';
    if (is_ip_literal(host)) {
        snprintf(out_ip, cap, "%s", host);
        return 0;
    }

    cache_lock();
    for (int i = 0; i < DNS_CACHE_SLOTS; ++i) {
        if (g_entries[i].used && strcmp(g_entries[i].host, host) == 0) {
            snprintf(out_ip, cap, "%s", g_entries[i].ip);
            cache_unlock();
            return 0;
        }
    }
    cache_unlock();

    /* 未命中：在调用线程解析一次（不在锁内，避免阻塞其他连接）。 */
    ip[0] = '\0';
    if (resolve_uncached(host, ip, sizeof(ip)) != 0) {
        zn_warn("dns: resolve %s failed", host);
        return -1;
    }

    cache_lock();
    {
        int idx = -1;

        for (int i = 0; i < DNS_CACHE_SLOTS; ++i) {
            if (g_entries[i].used && strcmp(g_entries[i].host, host) == 0) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            idx = g_next_slot;
            g_next_slot = (g_next_slot + 1) % DNS_CACHE_SLOTS;
            snprintf(g_entries[idx].host, sizeof(g_entries[idx].host), "%s", host);
            g_entries[idx].used = 1;
        }
        snprintf(g_entries[idx].ip, sizeof(g_entries[idx].ip), "%s", ip);
    }
    cache_unlock();

    snprintf(out_ip, cap, "%s", ip);
    return 0;
}

const char *zms_dns_target(const char *host, char *buf, size_t cap)
{
    if (!host || !buf || cap == 0) {
        return host;
    }
    if (zms_dns_resolve(host, buf, cap) == 0) {
        return buf;
    }
    return host;
}

void zms_dns_cache_warm(const char *url)
{
    const char *host;
    const char *p;
    char hostbuf[DNS_HOST_MAX];
    size_t n = 0;
    char ip[64];

    if (!url || !url[0]) {
        return;
    }
    host = strstr(url, "://");
    host = host ? host + 3 : url;
    /* 跳过 userinfo（user:pass@host） */
    p = strchr(host, '@');
    {
        const char *slash = strchr(host, '/');

        if (p && (!slash || p < slash)) {
            host = p + 1;
        }
    }
    /* IPv6 字面量形如 [::1]:8080 —— 先按方括号取，不能按冒号切。 */
    if (host[0] == '[') {
        const char *close = strchr(host, ']');

        if (!close) {
            return;
        }
        n = (size_t)(close - host - 1);
        if (n == 0 || n >= sizeof(hostbuf)) {
            return;
        }
        memcpy(hostbuf, host + 1, n);
        hostbuf[n] = '\0';
    } else {
        const char *slash = strchr(host, '/');
        const char *colon = strchr(host, ':');

        if (colon && (!slash || colon < slash)) {
            n = (size_t)(colon - host);
        } else if (slash) {
            n = (size_t)(slash - host);
        } else {
            n = strlen(host);
        }
        if (n == 0 || n >= sizeof(hostbuf)) {
            return;
        }
        memcpy(hostbuf, host, n);
        hostbuf[n] = '\0';
    }
    if (zms_dns_resolve(hostbuf, ip, sizeof(ip)) == 0) {
        zn_info("dns: warm %s -> %s", hostbuf, ip);
    }
}

void zms_dns_cache_flush(void)
{
    cache_lock();
    memset(g_entries, 0, sizeof(g_entries));
    g_next_slot = 0;
    cache_unlock();
}
