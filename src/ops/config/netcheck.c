/**
 * @file netcheck.c
 * @brief 本机网卡地址自检：用于校验 [general] externIP 是否还是本机地址。
 *
 * 为什么要这一步：WebRTC 的 ICE 候选与 SDP c= 行直接用 externIP。机器换网段、
 * 改 DHCP、从测试机搬到现场之后，配置里的旧 IP 不会报错，只会表现为
 * "信令成功但浏览器黑屏"——排查成本很高。启动时比对一次能把这个坑挡在前面。
 */
#include "zcs/ops/config/netcheck.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iptypes.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

int zcs_netcheck_host_ip_present(const char *ip)
{
    ULONG buf_len = 16 * 1024;
    IP_ADAPTER_ADDRESSES *buf = NULL;
    ULONG rc;
    int found = 0;
    int attempts;

    if (!ip || !ip[0]) {
        return 0;
    }
    for (attempts = 0; attempts < 3; ++attempts) {
        buf = (IP_ADAPTER_ADDRESSES *)malloc(buf_len);
        if (!buf) {
            return 0;
        }
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                               GAA_FLAG_SKIP_DNS_SERVER,
                                  NULL, buf, &buf_len);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            free(buf);
            buf = NULL;
            continue; /* buf_len 已被填充为所需大小 */
        }
        break;
    }
    if (!buf) {
        return 1; /* 无法枚举网卡：不判负，避免误报告警 */
    }
    if (rc == NO_ERROR) {
        IP_ADAPTER_ADDRESSES *a;
        for (a = buf; a && !found; a = a->Next) {
            IP_ADAPTER_UNICAST_ADDRESS *u;
            for (u = a->FirstUnicastAddress; u; u = u->Next) {
                struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
                char text[64];
                if (!sin || sin->sin_family != AF_INET) {
                    continue;
                }
                text[0] = '\0';
                if (InetNtopA(AF_INET, &sin->sin_addr, text, (DWORD)sizeof(text)) && text[0] &&
                    strcmp(text, ip) == 0) {
                    found = 1;
                    break;
                }
            }
        }
    }
    free(buf);
    return rc == NO_ERROR ? found : 1; /* 枚举失败时不判负 */
}

#else

#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int zcs_netcheck_host_ip_present(const char *ip)
{
    struct ifaddrs *ifa = NULL;
    struct ifaddrs *p;
    int found = 0;

    if (!ip || !ip[0]) {
        return 0;
    }
    if (getifaddrs(&ifa) != 0) {
        return 1; /* 无法枚举网卡：不判负，避免误报告警 */
    }
    for (p = ifa; p; p = p->ifa_next) {
        struct sockaddr_in *sin;
        char text[64];
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        sin = (struct sockaddr_in *)p->ifa_addr;
        text[0] = '\0';
        if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text)) && text[0] &&
            strcmp(text, ip) == 0) {
            found = 1;
            break;
        }
    }
    freeifaddrs(ifa);
    return found;
}

#endif
