/**
 * @file test_webrtc_rtcp.c
 * @brief WebRTC RTCP/TWCC 解析回归：NACK、PLI、复合包、TWCC 扩展。
 */
#include "protocol/webrtc/media_internal.h"
#include "zcs/media/wire/srtp.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

typedef struct {
    int count;
    size_t last_len;
} compound_ctx;

static void on_compound(const uint8_t *block, size_t block_len, void *user)
{
    compound_ctx *ctx = (compound_ctx *)user;
    if (block && block_len >= 4) {
        ++ctx->count;
        ctx->last_len = block_len;
    }
}

static int test_nack_pli(void)
{
    uint8_t nack[16] = {
        0x81, 205, 0x00, 0x03, /* V=2, FMT=1, PT=205, length=3 */
        0x01, 0x02, 0x03, 0x04, /* sender SSRC */
        0x05, 0x06, 0x07, 0x08, /* media SSRC */
        0x12, 0x34, 0x00, 0x03  /* PID=0x1234, BLP=0x0003 */
    };
    uint8_t pli[12] = {
        0x81, 206, 0x00, 0x01, /* V=2, FMT=1, PT=206, length=1 */
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
    };
    uint8_t bad[12] = {0x80, 205, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};

    CHECK(zcs_webrtc_rtcp_is_rtpfb_nack(nack, sizeof(nack)));
    CHECK(!zcs_webrtc_rtcp_is_psfb_pli(nack, sizeof(nack)));
    CHECK(zcs_webrtc_rtcp_is_psfb_pli(pli, sizeof(pli)));
    CHECK(!zcs_webrtc_rtcp_is_rtpfb_nack(pli, sizeof(pli)));
    CHECK(!zcs_webrtc_rtcp_is_rtpfb_nack(bad, sizeof(bad)));
    CHECK(!zcs_webrtc_rtcp_is_rtpfb_nack(nack, 8));
    return 0;
}

static int test_compound(void)
{
    uint8_t sr[28];
    uint8_t nack[16] = {
        0x81, 205, 0x00, 0x03, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x00, 0x01, 0x00, 0x00
    };
    uint8_t buf[64];
    compound_ctx ctx;
    size_t sr_len;

    sr_len = zcs_webrtc_rtcp_build_sr(sr, sizeof(sr), 0x01020304, 0, 0, 90000, 10, 1000);
    CHECK(sr_len >= 28);
    memcpy(buf, sr, sr_len);
    memcpy(buf + sr_len, nack, sizeof(nack));
    memset(&ctx, 0, sizeof(ctx));
    zcs_webrtc_rtcp_for_each_compound(buf, sr_len + sizeof(nack), on_compound, &ctx);
    CHECK(ctx.count == 2);
    CHECK(ctx.last_len == sizeof(nack));
    return 0;
}

static int test_twcc_ext(void)
{
    uint8_t rtp[64] = {0};
    uint8_t original[12] = {0x80, 96, 0x00, 0x01, 0, 0, 0, 1, 0, 0, 0, 2};
    size_t out_len = 0;

    memcpy(rtp, original, sizeof(original));
    memset(rtp + 12, 0xAA, 8);
    CHECK(zcs_webrtc_rtp_add_twcc_ext(rtp, sizeof(original) + 8, sizeof(rtp), 3, 0x1234,
                                      &out_len) == sizeof(original) + 8 + 8);
    CHECK(out_len == sizeof(original) + 8 + 8);
    CHECK((rtp[0] & 0x10) != 0);
    CHECK(rtp[12] == 0xBE && rtp[13] == 0xDE);
    CHECK(rtp[16] == ((3 << 4) | 1));
    CHECK(rtp[17] == 0x12 && rtp[18] == 0x34);
    CHECK(rtp[20] == 0xAA && rtp[21] == 0xAA && rtp[27] == 0xAA);
    return 0;
}

static int test_twcc_feedback(void)
{
    uint8_t twcc[24] = {
        0x8F, 205, 0x00, 0x05, /* V=2, FMT=15, PT=205, length=5 */
        0x01, 0x02, 0x03, 0x04, /* sender SSRC */
        0x05, 0x06, 0x07, 0x08, /* media SSRC */
        0x01, 0x00,             /* base seq = 256 */
        0x00, 0x08,             /* packet count = 8 */
        0x00, 0x00, 0x00,       /* reference time */
        0x01,                   /* feedback packet count */
        0x00, 0x05,             /* run length 5, status=not received */
        0xB8, 0x00              /* 1-bit vector: 3 received then idle */
    };
    zcs_webrtc_twcc_summary tw;

    CHECK(zcs_webrtc_rtcp_is_rtpfb_twcc(twcc, sizeof(twcc)));
    CHECK(!zcs_webrtc_rtcp_is_rtpfb_nack(twcc, sizeof(twcc)));
    CHECK(zcs_webrtc_rtcp_parse_twcc(twcc, sizeof(twcc), &tw) == 0);
    CHECK(tw.base_seq == 256);
    CHECK(tw.pkt_count == 8);
    CHECK(tw.parsed == 8);
    CHECK(tw.lost == 5);
    CHECK(tw.received == 3);
    CHECK(zcs_webrtc_rtcp_parse_twcc(twcc, 16, &tw) != 0);
    return 0;
}

static int test_rr(void)
{
    uint8_t rr[32] = {
        0x81, 201, 0x00, 0x07, /* V=2, RC=1, PT=201, length=7 */
        0x01, 0x02, 0x03, 0x04, /* sender SSRC */
        0x05, 0x06, 0x07, 0x08, /* source SSRC */
        0x19, 0x00, 0x00, 0x00, /* fraction lost=25, cumulative lost=0 */
        0x00, 0x00, 0x00, 0x64, /* extended highest seq */
        0x00, 0x00, 0x04, 0x00, /* jitter=1024 */
        0x11, 0x22, 0x33, 0x44, /* LSR */
        0x00, 0x00, 0x80, 0x00  /* DLSR */
    };
    uint32_t fraction = 0;
    uint32_t jitter = 0;
    uint32_t lsr = 0;
    uint32_t dlsr = 0;

    CHECK(zcs_webrtc_rtcp_parse_rr(rr, sizeof(rr), &fraction, &jitter, &lsr, &dlsr) == 0);
    CHECK(fraction == 25);
    CHECK(jitter == 1024);
    CHECK(lsr == 0x11223344u);
    CHECK(dlsr == 0x00008000u);
    CHECK(zcs_webrtc_rtcp_parse_rr(rr, 16, &fraction, &jitter, &lsr, &dlsr) != 0);
    return 0;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/** 用 libsrtp 生成的固定向量验证接收侧 SRTP（AES128_CM_SHA1_80）。 */
static int test_srtp_unprotect_vector(void)
{
    static const char *hex =
        "8000010210203040112233449d7d00b161d656daac8b1fb8661d81fc31c18e31422c977d18cdac"
        "051b2e4e0842f23af48800bf288e900cd59e0c1854d193a9a931ad96cd9853360846d20bca00d"
        "7b02644deae0802ed60aa3914cca46f0436de5b34596b7b8192701f01daa608fa21989e46b41f"
        "f5ada81cec15edd0f1f6e132db8b777490cb2865ca373fffda0b7b8cb5744e15b0e8d16a67d6e"
        "783bf04f4daeb6cd4b2a6d0cd003151b17143e57b5f200cc7b32283";
    uint8_t key[16];
    uint8_t salt[14];
    uint8_t pkt[256];
    uint8_t expect[256];
    size_t pkt_len = 0;
    size_t expect_len = 0;
    size_t i;
    zcs_webrtc_srtp *ctx;
    uint8_t scratch[4096];

    for (i = 0; i < strlen(hex); i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        CHECK(hi >= 0 && lo >= 0);
        CHECK(expect_len < sizeof(expect));
        expect[expect_len++] = (uint8_t)((hi << 4) | lo);
    }
    CHECK(expect_len == 182);
    for (i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i + 1);
    for (i = 0; i < sizeof(salt); ++i) salt[i] = (uint8_t)(0x11 + i);

    /* 接收：libsrtp 保护的包必须能还原。 */
    memcpy(pkt, expect, expect_len);
    pkt_len = expect_len;
    ctx = zcs_webrtc_srtp_create();
    CHECK(ctx != NULL);
    CHECK(zcs_webrtc_srtp_init_recv(ctx, key, salt) == 0);
    zcs_webrtc_srtp_bind_scratch(ctx, scratch, sizeof(scratch));
    CHECK(zcs_webrtc_srtp_unprotect(ctx, pkt, &pkt_len) == 0);
    CHECK(pkt_len == 172);
    CHECK(pkt[0] == 0x80 && pkt[1] == 0x00);
    CHECK(pkt[2] == 0x01 && pkt[3] == 0x02);
    CHECK(pkt[8] == 0x11 && pkt[9] == 0x22 && pkt[10] == 0x33 && pkt[11] == 0x44);
    for (i = 0; i < 160; ++i) {
        CHECK(pkt[12 + i] == (uint8_t)i);
    }
    zcs_webrtc_srtp_destroy(ctx);

    /* 别名暂存：会话 io_buf 同时作为报文与 HMAC 暂存，必须仍能校验 tag。 */
    {
        uint8_t io[4096];

        memcpy(io, expect, expect_len);
        pkt_len = expect_len;
        ctx = zcs_webrtc_srtp_create();
        CHECK(ctx != NULL);
        CHECK(zcs_webrtc_srtp_init_recv(ctx, key, salt) == 0);
        zcs_webrtc_srtp_bind_scratch(ctx, io, sizeof(io));
        CHECK(zcs_webrtc_srtp_unprotect(ctx, io, &pkt_len) == 0);
        CHECK(pkt_len == 172);
        for (i = 0; i < 160; ++i) {
            CHECK(io[12 + i] == (uint8_t)i);
        }
        zcs_webrtc_srtp_destroy(ctx);
    }

    /* 发送：pkt 此时已是解出的明文，同一密钥应得到与 libsrtp 相同的保护包。 */
    ctx = zcs_webrtc_srtp_create();
    CHECK(ctx != NULL);
    CHECK(zcs_webrtc_srtp_init_send(ctx, key, salt) == 0);
    zcs_webrtc_srtp_bind_scratch(ctx, scratch, sizeof(scratch));
    pkt_len = 172;
    CHECK(zcs_webrtc_srtp_protect(ctx, pkt, &pkt_len, sizeof(pkt)) == 0);
    CHECK(pkt_len == expect_len);
    if (memcmp(pkt, expect, expect_len) != 0) {
        for (i = 0; i < expect_len; ++i) {
            if (pkt[i] != expect[i]) {
                fprintf(stderr, "srtp protect diff at %zu: got=%02x want=%02x\n", i, pkt[i],
                        expect[i]);
                break;
            }
        }
        return 1;
    }
    zcs_webrtc_srtp_destroy(ctx);
    return 0;
}

static int test_srtcp_unprotect_vector(void)
{
    static const char *hex =
        "81c900070102030463d1f8c4c9c86aab1bdb29c4d826155c037d94399a51370c8000000151c75195e3"
        "35392fccbb";
    uint8_t key[16];
    uint8_t salt[14];
    uint8_t pkt[128];
    size_t pkt_len = 0;
    size_t i;
    zcs_webrtc_srtp *ctx;

    for (i = 0; i < strlen(hex); i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        CHECK(hi >= 0 && lo >= 0);
        CHECK(pkt_len < sizeof(pkt));
        pkt[pkt_len++] = (uint8_t)((hi << 4) | lo);
    }
    CHECK(pkt_len == 46);
    for (i = 0; i < sizeof(key); ++i) key[i] = (uint8_t)(i + 1);
    for (i = 0; i < sizeof(salt); ++i) salt[i] = (uint8_t)(0x11 + i);

    ctx = zcs_webrtc_srtp_create();
    CHECK(ctx != NULL);
    CHECK(zcs_webrtc_srtp_init_recv(ctx, key, salt) == 0);
    CHECK(zcs_webrtc_srtcp_unprotect(ctx, pkt, &pkt_len) == 0);
    CHECK(pkt_len == 32);
    CHECK(pkt[0] == 0x81 && pkt[1] == 201);
    CHECK(pkt[4] == 1 && pkt[8] == 5 && pkt[12] == 25);
    zcs_webrtc_srtp_destroy(ctx);
    return 0;
}

int main(void)
{
    CHECK(test_nack_pli() == 0);
    CHECK(test_compound() == 0);
    CHECK(test_twcc_ext() == 0);
    CHECK(test_twcc_feedback() == 0);
    CHECK(test_rr() == 0);
    CHECK(test_srtp_unprotect_vector() == 0);
    CHECK(test_srtcp_unprotect_vector() == 0);
    printf("test_webrtc_rtcp: ok\n");
    return 0;
}
