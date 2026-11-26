/**
 * @file test_gb_media_loopback.c
 * @brief GB28181 媒体发送回环：poller 驱动（无每路线程）UDP 发送 PS/RTP。
 *
 * 覆盖 6.1 改动的关键路径：
 *  - gb_media_start 只在 poller 上挂定时器，发送节拍由 poller 推进；
 *  - 等源上线重试 / 源就绪后建 muxer、挂读者、从 IDR 起播；
 *  - UDP 发送的 RTP 头字段与 PS 载荷；
 *  - gb_media_stop_call 收尾后会话计数归零（poller 仍在运行时安全摘除）。
 */
#include "device/gb28181/gb28181_internal.h"
#include "zms/core/frame.h"
#include "zms/core/gop/queue.h"
#include "zms/core/stream/hub.h"
#include "zn/platform.h"
#include "zn/poller/poller.h"
#include "zb/os/thread.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_INVALID INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_INVALID (-1)
#define sock_close close
#endif

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define CHANNEL_ID "34020000001320000001"
#define APP "gb"
#define STREAM "cam1"

/* 最小 H.264 Annex-B：IDR 帧带 SPS/PPS + IDR NAL，普通帧只有 P slice。 */
static const uint8_t k_idr[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E, 0x95, 0xA8, 0x28, 0x0F, 0x00,
    0x44, 0xFC, 0xB8, 0x08, 0x80, 0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x21, 0x00, 0x22, 0x33, 0x44,
    0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22,
};
static const uint8_t k_p[] = {
    0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x00, 0x22, 0x33, 0x44, 0x55, 0x66,
    0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x12, 0x34, 0x56,
};

static volatile int g_stop;

static void poller_main(void *user)
{
    zn_poller_run((zn_poller *)user, &g_stop);
}

static void push_frames(zms_gop_queue *q, int count, int key_every)
{
    int i;

    for (i = 0; i < count; ++i) {
        zms_frame f;
        int key = key_every > 0 && (i % key_every) == 0;

        memset(&f, 0, sizeof(f));
        f.data = (uint8_t *)(key ? k_idr : k_p);
        f.size = key ? sizeof(k_idr) : sizeof(k_p);
        f.dts_ms = (uint32_t)i * 40u;
        f.pts_ms = f.dts_ms;
        f.codec = ZMS_CODEC_H264;
        f.track = ZMS_TRACK_VIDEO;
        f.keyframe = key ? 1 : 0;
        (void)zms_gop_queue_write(q, &f);
    }
}

int main(void)
{
    zn_poller *p = NULL;
    zb_thread *th = NULL;
    zms_media_source *src;
    gb_media_session *sess;
    gb_channel ch;
    sock_t rx = SOCK_INVALID;
    struct sockaddr_in bind_addr;
    struct sockaddr_in peer;
    socklen_t alen;
    uint16_t rx_port;
    int local_port = 0;
    int i;
    int got = 0;
    int saw_ps = 0;
    int saw_idr_pts0 = 0;
    int rc = 1;

#if defined(_WIN32)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif
    zn_platform_init();
    zms_media_source_registry_init();

    /* 1) 已发布的直播源 + GB 通道映射。 */
    src = zms_media_source_register("rtmp", APP, STREAM);
    CHECK(src != NULL);
    src->publishing = 1;
    src->has_video = 1;
    src->video.codec = ZMS_CODEC_H264;
    src->video.ready = 1;
    src->video.width = 1280;
    src->video.height = 720;
    src->video.fps = 25.0f;
    CHECK(gb_channels_load(CHANNEL_ID ":" APP "/" STREAM) == 1);
    CHECK(gb_channel_get(CHANNEL_ID, &ch) == 0);

    push_frames(src->gop_queue, 30, 10); /* 先有 3 个 GOP，保证能选到 IDR */

    /* 2) UDP 接收端（模拟平台侧媒体口）。 */
    rx = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(rx != SOCK_INVALID);
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = 0;
    CHECK(bind(rx, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) == 0);
    alen = sizeof(bind_addr);
    CHECK(getsockname(rx, (struct sockaddr *)&bind_addr, &alen) == 0);
    rx_port = ntohs(bind_addr.sin_port);

    /* 3) poller 线程 + 无 GB 上下文时注入 poller。 */
    p = zn_poller_create();
    CHECK(p != NULL);
    th = zb_thread_create(poller_main, p);
    CHECK(th != NULL);
    for (i = 0; i < 200 && !zn_poller_is_running(p); ++i) {
        zb_sleep_ms(1);
    }
    CHECK(zn_poller_is_running(p));
    gb_media_set_poller_for_test(p);

    sess = gb_media_open("call-loopback-1", CHANNEL_ID, "34020000002000000001", 0x0A0B0C0Du,
                         96, GB_MEDIA_UDP, "127.0.0.1", rx_port, &local_port);
    CHECK(sess != NULL);
    CHECK(local_port > 0);
    CHECK(gb_media_session_count() == 1);
    CHECK(gb_media_start(sess, &ch) == 0);

    /* 4) 收包：跨 ~2 秒持续推帧，验证 RTP 头与 PS 起始码。 */
    for (i = 0; i < 100 && (!saw_ps || got < 4); ++i) {
        uint8_t pkt[2048];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n;

        push_frames(src->gop_queue, 2, 0);
        zn_poller_wake(p);
#if defined(_WIN32)
        {
            DWORD tv_ms = 20;
            setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
        }
#else
        {
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 20000;
            setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
#endif
        n = (int)recvfrom(rx, (char *)pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &flen);
        if (n < 12) {
            continue;
        }
        ++got;
        /* RTP: V=2, PT=96, SSRC 与协商一致。 */
        CHECK((pkt[0] >> 6) == 2);
        CHECK((pkt[1] & 0x7F) == 96);
        CHECK(((uint32_t)pkt[8] << 24 | (uint32_t)pkt[9] << 16 | (uint32_t)pkt[10] << 8 |
               (uint32_t)pkt[11]) == 0x0A0B0C0Du);
        if (n >= 16 && pkt[12] == 0x00 && pkt[13] == 0x00 && pkt[14] == 0x01 && pkt[15] == 0xBA) {
            saw_ps = 1;
            saw_idr_pts0 = 1;
        }
        CHECK(ntohs(from.sin_port) == (uint16_t)local_port); /* 源端口 = 会话本地口 */
    }
    CHECK(got >= 4);
    CHECK(saw_ps);
    CHECK(saw_idr_pts0);

    /* 5) 运行中停止：收尾在 poller 线程执行（异步），等待摘链完成。 */
    CHECK(gb_media_stop_call("call-loopback-1") == 1);
    for (i = 0; i < 200 && gb_media_session_count() != 0; ++i) {
        zn_poller_wake(p);
        zb_sleep_ms(5);
    }
    CHECK(gb_media_session_count() == 0);
    /* 再调一次必须安全（已不存在）。 */
    CHECK(gb_media_stop_call("call-loopback-1") == 0);

    /* 6) TCP 被动模式：平台侧连入，验证 2 字节长度前缀 + 分帧散写。 */
    {
        sock_t tc = SOCK_INVALID;
        uint8_t buf[4096];
        size_t have = 0;
        int got_tcp = 0;
        int saw_ps_tcp = 0;

        sess = gb_media_open("call-loopback-tcp", CHANNEL_ID, "34020000002000000001", 0x0A0B0C0Eu,
                             96, GB_MEDIA_TCP_PASSIVE, "127.0.0.1", 0, &local_port);
        CHECK(sess != NULL);
        CHECK(local_port > 0);
        CHECK(gb_media_start(sess, &ch) == 0);

        zn_poller_wake(p);
        zb_sleep_ms(50); /* 等 accept 事件挂上监听 */

        tc = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(tc != SOCK_INVALID);
        memset(&peer, 0, sizeof(peer));
        peer.sin_family = AF_INET;
        peer.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        peer.sin_port = htons((uint16_t)local_port);
        for (i = 0; i < 100; ++i) { /* poller 先挂监听，连不上就重试 */
            if (connect(tc, (struct sockaddr *)&peer, sizeof(peer)) == 0) {
                break;
            }
            zb_sleep_ms(10);
        }
        CHECK(i < 100);
#if defined(_WIN32)
        {
            DWORD tv_ms = 5000;
            setsockopt(tc, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv_ms, sizeof(tv_ms));
        }
#else
        {
            struct timeval tv;
            tv.tv_sec = 5;
            tv.tv_usec = 0;
            setsockopt(tc, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
#endif
        /* accept 完成后开始推帧并收分帧。 */
        for (i = 0; i < 200 && got_tcp < 4; ++i) {
            int n;

            push_frames(src->gop_queue, 2, 0);
            zn_poller_wake(p);
            n = (int)recv(tc, (char *)buf + have, (int)(sizeof(buf) - have), 0);
            if (n <= 0) {
                if (n == 0) {
                    break;
                }
                continue;
            }
            have += (size_t)n;
            while (have >= 2) {
                size_t flen = ((size_t)buf[0] << 8) | buf[1];
                const uint8_t *rtp;

                if (have < 2 + flen) {
                    break;
                }
                rtp = buf + 2;
                CHECK(flen >= 12);
                CHECK((rtp[0] >> 6) == 2);
                CHECK((rtp[1] & 0x7F) == 96);
                CHECK(((uint32_t)rtp[8] << 24 | (uint32_t)rtp[9] << 16 |
                       (uint32_t)rtp[10] << 8 | (uint32_t)rtp[11]) == 0x0A0B0C0Eu);
                if (flen >= 16 && rtp[12] == 0x00 && rtp[13] == 0x00 && rtp[14] == 0x01 &&
                    rtp[15] == 0xBA) {
                    saw_ps_tcp = 1;
                }
                ++got_tcp;
                memmove(buf, buf + 2 + flen, have - 2 - flen);
                have -= 2 + flen;
            }
        }
        sock_close(tc);
        CHECK(got_tcp >= 4);
        CHECK(saw_ps_tcp);
        CHECK(gb_media_stop_call("call-loopback-tcp") == 1);
        for (i = 0; i < 200 && gb_media_session_count() != 0; ++i) {
            zn_poller_wake(p);
            zb_sleep_ms(5);
        }
        CHECK(gb_media_session_count() == 0);
    }

    g_stop = 1;
    zn_poller_wake(p);
    CHECK(zb_thread_join(th) == ZB_OK);
    zb_thread_destroy(th);
    zn_poller_destroy(p);
    gb_media_set_poller_for_test(NULL);

    sock_close(rx);
    zms_media_source_close(src, 1);
    (void)peer;
    printf("test_gb_media_loopback ok (rtp=%d ps=%d port=%u)\n", got, saw_ps,
           (unsigned)local_port);
    rc = 0;
    return rc;
}
