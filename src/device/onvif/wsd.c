/**
 * @file wsd.c
 * @brief WS-Discovery：发送 Probe 并收集 ProbeMatch。
 */
#include "device/onvif/onvif_internal.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET zcs_wsd_sock_t;
#define ZCS_WSD_INVALID INVALID_SOCKET
#define zcs_wsd_close closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int zcs_wsd_sock_t;
#define ZCS_WSD_INVALID (-1)
#define zcs_wsd_close close
#endif

#define ZCS_WSD_GROUP "239.255.255.250"
#define ZCS_WSD_PORT 3702

static const char k_probe_template[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<e:Envelope xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\""
    " xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\""
    " xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\""
    " xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
    "<e:Header>"
    "<w:MessageID>%s</w:MessageID>"
    "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
    "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
    "</e:Header>"
    "<e:Body><d:Probe><d:Types>dn:NetworkVideoTransmitter</d:Types></d:Probe></e:Body>"
    "</e:Envelope>";

static void wsd_set_timeout(zcs_wsd_sock_t fd, int timeout_ms)
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

static int copy_first_url(const char *text, char *out, size_t cap)
{
    const char *p = text;
    size_t len = 0;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        ++p;
    }
    while (p[len] && p[len] != ' ' && p[len] != '\t' && p[len] != '\r' && p[len] != '\n') {
        ++len;
    }
    if (len == 0 || len >= cap) {
        return -1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

/** 在 ProbeMatch XML 中提取 xaddr/uuid/scopes */
static int parse_probe_match(const char *xml, zcs_onvif_device *dev)
{
    char addrs[512];
    const char *match;

    match = zcs_xml_find(xml, "ProbeMatch");
    if (!match) {
        return -1;
    }
    if (zcs_xml_text_between(match, NULL, "XAddrs", addrs, sizeof(addrs)) != 0) {
        return -1;
    }
    if (copy_first_url(addrs, dev->xaddr, sizeof(dev->xaddr)) != 0) {
        return -1;
    }
    if (strncmp(dev->xaddr, "http://", 7) != 0) {
        return -1;
    }
    (void)zcs_xml_text_between(match, NULL, "Address", dev->uuid, sizeof(dev->uuid));
    (void)zcs_xml_text_between(match, NULL, "Scopes", dev->scopes, sizeof(dev->scopes));
    if (dev->name[0] == '\0') {
        const char *name = strstr(dev->scopes, "/name/");
        if (name) {
            size_t len = strcspn(name + 6, " /");
            if (len > 0 && len < sizeof(dev->name)) {
                memcpy(dev->name, name + 6, len);
                dev->name[len] = '\0';
            }
        }
    }
    return 0;
}

int zcs_onvif_discover(int timeout_ms, zcs_onvif_device *out, size_t cap, size_t *count)
{
    zcs_wsd_sock_t fd;
    struct sockaddr_in addr;
    char msgid[80];
    char probe[2048];
    int probe_len;
    uint64_t deadline;
    int ttl = 2;
    size_t found = 0;

    if (count) {
        *count = 0;
    }
    if (!out || cap == 0) {
        return -1;
    }
    if (timeout_ms <= 0) {
        timeout_ms = 3000;
    }

#if defined(_WIN32)
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
#endif

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == ZCS_WSD_INVALID) {
        zn_warn("onvif: discovery socket failed");
        return -1;
    }
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *)&ttl, sizeof(ttl));

    zcs_onvif_make_msgid(msgid, sizeof(msgid));
    probe_len = snprintf(probe, sizeof(probe), k_probe_template, msgid);
    if (probe_len <= 0 || (size_t)probe_len >= sizeof(probe)) {
        zcs_wsd_close(fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ZCS_WSD_PORT);
    inet_pton(AF_INET, ZCS_WSD_GROUP, &addr.sin_addr);
    if (sendto(fd, probe, probe_len, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        zn_warn("onvif: discovery sendto failed");
        zcs_wsd_close(fd);
        return -1;
    }

    wsd_set_timeout(fd, 300);
    deadline = zn_monotonic_ms() + (uint64_t)timeout_ms;
    while (zn_monotonic_ms() < deadline) {
        char buf[8192];
        int n;
#if defined(_WIN32)
        n = recvfrom(fd, buf, (int)sizeof(buf) - 1, 0, NULL, NULL);
#else
        n = (int)recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
#endif
        if (n <= 0) {
            continue; /* 超时继续等待直到 deadline */
        }
        buf[n] = '\0';
        if (found < cap) {
            zcs_onvif_device dev;
            size_t i;
            int dup = 0;
            memset(&dev, 0, sizeof(dev));
            if (parse_probe_match(buf, &dev) == 0) {
                for (i = 0; i < found; ++i) {
                    if (strcmp(out[i].xaddr, dev.xaddr) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    out[found++] = dev;
                }
            }
        }
    }
    zcs_wsd_close(fd);
    if (count) {
        *count = found;
    }
    return 0;
}
