/**
 * @file http_client.c
 * @brief 极简同步 HTTP 客户端实现（见 http_client.h）。
 */
#include "zms/util/http_client.h"
#include "zn/util/log.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET zms_hsock_t;
#define ZMS_HSOCK_INVALID INVALID_SOCKET
#define zms_hsock_close closesocket
typedef int zms_socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int zms_hsock_t;
#define ZMS_HSOCK_INVALID (-1)
#define zms_hsock_close close
typedef socklen_t zms_socklen_t;
#endif

#define ZMS_HTTP_DEFAULT_TIMEOUT_MS 10000
/* 上限既是防御（对端恶意/异常时我们不能无限 malloc），也是简化：
 * 头部 64K、正文 16M 覆盖了 webhook/配置拉取等本客户端的全部实际用途。
 * 超出即停止接收、按已收内容尽力解析。 */
#define ZMS_HTTP_MAX_HEADER (64 * 1024)
#define ZMS_HTTP_MAX_BODY (16 * 1024 * 1024)

#if defined(_WIN32)
static int hsock_last_error(void)
{
    return WSAGetLastError();
}

/** ZMS/ZN 内部可能尚未初始化 Winsock（如独立 CLI 直接发起请求） */
static void hsock_global_init(void)
{
    static int inited;
    if (!inited) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        inited = 1;
    }
}
#else
static int hsock_last_error(void)
{
    return errno;
}

static void hsock_global_init(void)
{
}
#endif

static void hsock_set_timeouts(zms_hsock_t fd, int timeout_ms)
{
#if defined(_WIN32)
    DWORD tv = (DWORD)timeout_ms;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

static int hsock_set_nonblock(zms_hsock_t fd, int on)
{
#if defined(_WIN32)
    u_long mode = on ? 1u : 0u;
    return ioctlsocket(fd, FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0 ? 0 : -1;
#endif
}

/** 非阻塞 connect + select 超时，成功后恢复阻塞模式 */
/*
 * 为什么要绕这一圈：阻塞 connect 对内网黑洞地址会卡到系统级 SYN 超时
 * （Linux 约 2 分钟、Windows 更长），把我们自己的超时设置架空。改成非阻塞
 * connect，用 select 监听可写/异常集合来强制执行 timeout_ms。
 * 注意异常集合也必须监听：连接被拒时 fd 会出现在 efds 而非 wfds。
 */
static int hsock_connect_timeout(zms_hsock_t fd, const struct sockaddr *addr, socklen_t addr_len,
                                 int timeout_ms)
{
    fd_set wfds;
    fd_set efds;
    struct timeval tv;
    int rc;

    if (hsock_set_nonblock(fd, 1) != 0) {
        return -1;
    }
    rc = connect(fd, addr, addr_len);
    if (rc == 0) {
        (void)hsock_set_nonblock(fd, 0);
        return 0;
    }
    if (hsock_last_error() !=
#if defined(_WIN32)
        WSAEWOULDBLOCK
#else
        EINPROGRESS
#endif
    ) {
        return -1;
    }

    FD_ZERO(&wfds);
    FD_ZERO(&efds);
    FD_SET(fd, &wfds);
    FD_SET(fd, &efds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
#if defined(_WIN32)
    rc = select(0, NULL, &wfds, &efds, &tv);
#else
    rc = select(fd + 1, NULL, &wfds, &efds, &tv);
#endif
    if (rc <= 0 || FD_ISSET(fd, &efds)) {
        return -1;
    }
    (void)hsock_set_nonblock(fd, 0);
    return 0;
}

/* 循环写直到写满：send 可能只接受部分字节（发送缓冲区满），单次调用不能
 * 假定 len 一次写完；n<=0 且这是阻塞 socket 时代表出错（超时/断开）。 */
static int hsock_send_all(zms_hsock_t fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        int n;
#if defined(_WIN32)
        n = send(fd, buf + off, (int)(len - off), 0);
#else
        n = (int)send(fd, buf + off, len - off, 0);
#endif
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static int parse_url(const char *url, char *host, size_t host_cap, uint16_t *port, char *path,
                     size_t path_cap)
{
    const char *p;
    const char *slash;
    const char *colon;

    if (!url || strncmp(url, "http://", 7) != 0) {
        return -1;
    }
    p = url + 7;
    slash = strchr(p, '/');
    if (!slash) {
        slash = p + strlen(p);
    }
    colon = memchr(p, ':', (size_t)(slash - p));
    if (colon) {
        size_t host_len = (size_t)(colon - p);
        if (host_len >= host_cap) {
            return -1;
        }
        memcpy(host, p, host_len);
        host[host_len] = '\0';
        *port = (uint16_t)atoi(colon + 1);
    } else {
        size_t host_len = (size_t)(slash - p);
        if (host_len >= host_cap) {
            return -1;
        }
        memcpy(host, p, host_len);
        host[host_len] = '\0';
        *port = 80;
    }
    if (*slash == '\0') {
        strncpy(path, "/", path_cap - 1);
        path[path_cap - 1] = '\0';
    } else {
        strncpy(path, slash, path_cap - 1);
        path[path_cap - 1] = '\0';
    }
    return host[0] ? 0 : -1;
}

static int ascii_ieq_n(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb) {
            return 0;
        }
    }
    return 1;
}

static int contains_ci(const char *s, size_t len, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i;
    if (nl == 0 || len < nl) {
        return 0;
    }
    for (i = 0; i + nl <= len; ++i) {
        if (ascii_ieq_n(s + i, needle, nl)) {
            return 1;
        }
    }
    return 0;
}

static const char *find_header(const char *headers, const char *name)
{
    size_t name_len = strlen(name);
    const char *line = headers;

    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        if (!eol) {
            break;
        }
        if ((size_t)(eol - line) > name_len && line[name_len] == ':' &&
            ascii_ieq_n(line, name, name_len)) {
            return line + name_len + 1;
        }
        line = eol + 2;
    }
    return NULL;
}

/** 判断响应是否已完整收到（按 Content-Length / chunked 终止块），
 *  避免对 keep-alive 服务端一直等到读超时。 */
/*
 * 本客户端发的是 “Connection: close”，按理会读到 EOF；但不少服务端/反向
 * 代理会忽略该头或改成 keep-alive，此时 recv 不会返回 0，只能靠长度/终止块
 * 判断收完，否则会白等到超时才返回。chunked 的结束标记是 “0\r\n\r\n”。
 */
static int http_response_complete(const char *raw, size_t len)
{
    const char *he = strstr(raw, "\r\n\r\n");
    size_t header_len;
    const char *line;
    size_t name_len;

    if (!he) {
        return 0;
    }
    header_len = (size_t)(he - raw) + 4;
    line = raw;
    name_len = strlen("Content-Length");
    while (line < he) {
        const char *eol = strstr(line, "\r\n");
        if (!eol || eol > he) {
            break;
        }
        if ((size_t)(eol - line) > name_len && line[name_len] == ':' &&
            ascii_ieq_n(line, "Content-Length", name_len)) {
            size_t want = (size_t)strtoul(line + name_len + 1, NULL, 10);
            return len >= header_len + want;
        }
        if ((size_t)(eol - line) > strlen("Transfer-Encoding") &&
            ascii_ieq_n(line, "Transfer-Encoding", strlen("Transfer-Encoding")) &&
            line[strlen("Transfer-Encoding")] == ':' &&
            contains_ci(line, (size_t)(eol - line), "chunked")) {
            const char *body = raw + header_len;
            size_t body_len = len - header_len;
            if (body_len >= 5 && (strncmp(body, "0\r\n\r\n", 5) == 0 ||
                                  strstr(body, "\r\n0\r\n\r\n") != NULL)) {
                return 1;
            }
            return 0;
        }
        line = eol + 2;
    }
    return 0;
}

static char *trim_dup(const char *s, size_t len)
{
    char *out;
    while (len > 0 && (*s == ' ' || *s == '\t')) {
        ++s;
        --len;
    }
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) {
        --len;
    }
    out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/** 解析 chunked 编码；返回 malloc 的正文（失败 NULL） */
/*
 * 容量取 len+1 足够：解码只会去掉块头/块尾的元数据，所得正文必然不超过
 * 原始字节数，所以无需二次预估。遇到任何不完整/异常块就 break，把已解出的
 * 前缀作为正文返回——宁可返回截断内容，也不让调用方完全拿不到。
 */
static char *decode_chunked(const char *data, size_t len, size_t *out_len)
{
    size_t pos = 0;
    size_t cap = len + 1;
    char *out = (char *)malloc(cap);
    size_t used = 0;

    if (!out) {
        return NULL;
    }
    while (pos < len) {
        char *endp = NULL;
        unsigned long chunk;
        const char *line_end = strstr(data + pos, "\r\n");
        if (!line_end) {
            break;
        }
        chunk = strtoul(data + pos, &endp, 16);
        if (endp == data + pos) {
            break;
        }
        pos = (size_t)(line_end - data) + 2;
        if (chunk == 0) {
            break;
        }
        if (pos + chunk > len) {
            break;
        }
        memcpy(out + used, data + pos, chunk);
        used += chunk;
        pos += chunk;
        if (pos + 2 <= len) {
            pos += 2;
        }
    }
    out[used] = '\0';
    if (out_len) {
        *out_len = used;
    }
    return out;
}

void zms_http_resp_free(zms_http_resp *resp)
{
    if (!resp) {
        return;
    }
    free(resp->body);
    free(resp->headers);
    memset(resp, 0, sizeof(*resp));
}

int zms_http_exec(const zms_http_req *req, zms_http_resp *resp)
{
    char host[256];
    char path[1024];
    uint16_t port = 0;
    int timeout_ms;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char port_str[16];
    zms_hsock_t fd = ZMS_HSOCK_INVALID;
    char *raw = NULL;
    size_t raw_len = 0;
    size_t raw_cap = 0;
    char *hdr_end;
    int rc = -1;
    const char *method;

    if (!req || !resp || !req->url) {
        return -1;
    }
    memset(resp, 0, sizeof(*resp));
    resp->status = -1;
    resp->err = -1;
    hsock_global_init();

    if (parse_url(req->url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        zn_warn("http: unsupported url: %s", req->url);
        return -1;
    }
    timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : ZMS_HTTP_DEFAULT_TIMEOUT_MS;
    method = req->method && req->method[0] ? req->method : "POST";

    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        zn_warn("http: resolve failed host=%s", host);
        return -1;
    }

    /* 遍历 getaddrinfo 返回的地址逐个尝试：一个主机名可能同时解析出 IPv6
     * 与 IPv4，首个地址连接失败（如无 IPv6 路由）不应直接放弃，换下一个。 */
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == ZMS_HSOCK_INVALID) {
            continue;
        }
        if (hsock_connect_timeout(fd, ai->ai_addr, (zms_socklen_t)ai->ai_addrlen, timeout_ms) != 0) {
            zms_hsock_close(fd);
            fd = ZMS_HSOCK_INVALID;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    if (fd == ZMS_HSOCK_INVALID) {
        zn_warn("http: connect failed %s:%u", host, (unsigned)port);
        return -1;
    }
    hsock_set_timeouts(fd, timeout_ms);

    {
        size_t body_len = req->body ? (req->body_len ? req->body_len : strlen(req->body)) : 0;
        size_t head_cap = 4096 + (req->extra_headers ? strlen(req->extra_headers) : 0);
        char *head = (char *)malloc(head_cap);
        int n;

        if (!head) {
            zms_hsock_close(fd);
            return -1;
        }
        n = snprintf(head, head_cap,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%u\r\n"
                     "User-Agent: zms-http/0.5\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "%s%s%s"
                     "%s"
                     "Content-Length: %zu\r\n"
                     "\r\n",
                     method, path, host, (unsigned)port,
                     req->content_type ? "Content-Type: " : "",
                     req->content_type ? req->content_type : "",
                     req->content_type ? "\r\n" : "",
                     req->extra_headers ? req->extra_headers : "", body_len);
        if (n < 0 || (size_t)n >= head_cap) {
            free(head);
            zms_hsock_close(fd);
            return -1;
        }
        /* 头与正文分两次发送，避免为大体再拷贝一份 */
        /* 头里固定带 “Connection: close”：本客户端一次性用完即关，不实现
         * 连接复用；让服务端发完即断，简化读循环的结束判定。 */
        rc = hsock_send_all(fd, head, (size_t)n);
        free(head);
        if (rc == 0 && body_len > 0) {
            rc = hsock_send_all(fd, req->body, body_len);
        }
        if (rc != 0) {
            zms_hsock_close(fd);
            return -1;
        }
    }

    /* 读满整个响应。三种退出条件：
     *   n<0  → recv 出错（多为 SO_RCVTIMEO 超时）：按已收内容尽力解析，
     *          不直接判失败，这样慢服务端返回的完整响应仍能被接受；
     *   n==0 → 对端关闭，正常结束；
     *   完整 → http_response_complete 识别到 Content-Length/chunked 终止。
     * 缓冲区按需倍增（初始 8K），并受 ZMS_HTTP_MAX_BODY 硬上限约束。 */
    for (;;) {
        char buf[4096];
        int n;
#if defined(_WIN32)
        n = recv(fd, buf, (int)sizeof(buf), 0);
#else
        n = (int)recv(fd, buf, sizeof(buf), 0);
#endif
        if (n < 0) {
            break; /* 超时/错误：按已收内容尽力解析 */
        }
        if (n == 0) {
            break;
        }
        if (raw_len + (size_t)n + 1 > ZMS_HTTP_MAX_BODY) {
            break;
        }
        if (raw_len + (size_t)n + 1 > raw_cap) {
            size_t ncap = raw_cap ? raw_cap * 2 : 8192;
            char *nr;
            while (ncap < raw_len + (size_t)n + 1) {
                ncap *= 2;
            }
            nr = (char *)realloc(raw, ncap);
            if (!nr) {
                break;
            }
            raw = nr;
            raw_cap = ncap;
        }
        memcpy(raw + raw_len, buf, (size_t)n);
        raw_len += (size_t)n;
        raw[raw_len] = '\0';
        if (http_response_complete(raw, raw_len)) {
            break; /* 已完整（keep-alive 服务端不会主动关闭） */
        }
    }
    zms_hsock_close(fd);

    if (!raw || raw_len == 0) {
        free(raw);
        zn_warn("http: empty response %s", req->url);
        return -1;
    }
    hdr_end = strstr(raw, "\r\n\r\n");
    if (!hdr_end) {
        free(raw);
        zn_warn("http: bad response (no header end) %s", req->url);
        return -1;
    }
    /* 状态行形如 "HTTP/1.x NNN ..."：前缀固定 9 字节（"HTTP/" + 一位主版本 +
     * '.' + 一位次版本 + 空格），故从 raw+9 起 atoi 取三位状态码。取不到
     * 正数说明不是我们认识的 HTTP 响应，直接判失败。 */
    resp->status = atoi(raw + 9); /* "HTTP/1.x " */
    if (resp->status <= 0) {
        resp->status = -1;
        free(raw);
        return -1;
    }
    resp->headers = trim_dup(raw, (size_t)(hdr_end - raw) + 2);
    {
        const char *body = hdr_end + 4;
        size_t body_avail = raw_len - (size_t)(body - raw);
        const char *te = resp->headers ? find_header(resp->headers, "Transfer-Encoding") : NULL;

        /* chunked 优先于 Content-Length：按 RFC，Transfer-Encoding 存在时应
         * 忽略 Content-Length。非 chunked 时按 Content-Length 截取，但绝不
         * 超过实际收到的字节（want>body_avail 时钳制），防止服务端谎报长度
         * 导致越界读。 */
        if (te && strstr(te, "chunked")) {
            resp->body = decode_chunked(body, body_avail, &resp->body_len);
        } else {
            const char *cl = resp->headers ? find_header(resp->headers, "Content-Length") : NULL;
            size_t want = cl ? (size_t)strtoul(cl, NULL, 10) : body_avail;
            if (want > body_avail) {
                want = body_avail;
            }
            resp->body = (char *)malloc(want + 1);
            if (resp->body) {
                memcpy(resp->body, body, want);
                resp->body[want] = '\0';
                resp->body_len = want;
            }
        }
    }
    free(raw);
    resp->err = resp->body ? 0 : -1;
    if (resp->err != 0) {
        zn_warn("http: OOM decoding body %s", req->url);
        return -1;
    }
    return 0;
}
