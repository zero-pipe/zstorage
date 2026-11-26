/**
 * @file test_rtmp_handshake.c
 * @brief RTMP 握手状态机回归：C2 与后续 connect 数据同包到达。
 *
 * 真实客户端（ffmpeg）常把 C2 与 connect 命令头合并在同一个 TCP 段里。
 * 握手状态机在 C2 处理完后必须把剩余字节交回 chunk 解析，而不是继续在
 * 握手循环里空转（修复前该路径会死循环，导致 poller 线程 100% CPU 卡死，
 * 其上所有会话不再收发）。
 */
#include "zms/protocol/rtmp/protocol.h"
#include <stdio.h>
#include <string.h>

static int g_cmd_count;
static char g_last_cmd[32];

static void on_cmd(const char *cmd, double trans_id, const uint8_t *amf_rest, size_t amf_rest_len,
                   void *user)
{
    (void)trans_id;
    (void)amf_rest;
    (void)amf_rest_len;
    (void)user;
    ++g_cmd_count;
    if (cmd) {
        strncpy(g_last_cmd, cmd, sizeof(g_last_cmd) - 1);
        g_last_cmd[sizeof(g_last_cmd) - 1] = '\0';
    }
}

int main(void)
{
    zms_rtmp_protocol_opts opts;
    zms_rtmp_protocol *p;
    uint8_t c0c1[1 + 1536];
    uint8_t reply[1 + 1536 * 2];
    size_t reply_len = 0;
    /* AMF0 connect: string "connect" + number 1.0 + empty object */
    static const uint8_t amf_connect[] = {
        0x02, 0x00, 0x07, 'c', 'o', 'n', 'n', 'e', 'c', 't',
        0x00, 0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x09,
    };
    uint8_t buf[1536 + 12 + sizeof(amf_connect)];
    uint8_t *c = buf + 1536;

    memset(&opts, 0, sizeof(opts));
    opts.on_cmd = on_cmd;
    p = zms_rtmp_protocol_create(&opts);
    if (!p)
        return 1;

    memset(c0c1, 0, sizeof(c0c1));
    c0c1[0] = 3;
    if (zms_rtmp_protocol_input(p, c0c1, sizeof(c0c1)) != ZN_OK)
        return 2;
    if (!zms_rtmp_protocol_handshake_pending(p))
        return 3;
    if (zms_rtmp_protocol_send_handshake_s0s1s2(p, reply, sizeof(reply), &reply_len) != ZN_OK)
        return 4;
    if (reply_len != 1 + 1536 * 2)
        return 5;

    memset(buf, 0, 1536); /* C2 */
    c[0] = 0x03;          /* fmt=0, csid=3 */
    c[1] = 0;
    c[2] = 0;
    c[3] = 0;
    c[4] = 0;
    c[5] = 0;
    c[6] = (uint8_t)sizeof(amf_connect);
    c[7] = 20; /* AMF0 command */
    c[8] = 0;
    c[9] = 0;
    c[10] = 0;
    c[11] = 0;
    memcpy(c + 12, amf_connect, sizeof(amf_connect));

    /* 修复前：这里死循环 */
    if (zms_rtmp_protocol_input(p, buf, sizeof(buf)) != ZN_OK)
        return 6;
    if (!zms_rtmp_protocol_handshake_complete(p))
        return 7;
    if (g_cmd_count != 1 || strcmp(g_last_cmd, "connect") != 0)
        return 8;

    zms_rtmp_protocol_destroy(p);
    printf("test_rtmp_handshake ok\n");
    return 0;
}
