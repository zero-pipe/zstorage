/**
 * @file srtp.c
 * @brief WebRTC SRTP/SRTCP 包变换。
 */
#include "zms/media/media_limits.h"
#include "zms/media/wire/srtp.h"
#include "zms/media/wire/rtp_packet.h"
#include "zn/util/log.h"
#include "zn/platform.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#if defined(ZMS_HAVE_WEBRTC_DTLS) && ZMS_HAVE_WEBRTC_DTLS
#include <openssl/evp.h>
#include <openssl/hmac.h>
struct zms_webrtc_srtp {
    uint8_t enc_key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t auth_key[20];
    uint8_t session_salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    /* RFC 3711 §4.3.2：SRTCP 使用 label 3/4/5 派生独立密钥。 */
    uint8_t rtcp_enc_key[ZMS_WEBRTC_SRTP_KEY_LEN];
    uint8_t rtcp_auth_key[20];
    uint8_t rtcp_session_salt[ZMS_WEBRTC_SRTP_SALT_LEN];
    uint8_t *auth_send_buf;
    uint8_t *auth_recv_buf;
    size_t auth_buf_cap;
    EVP_CIPHER_CTX *ecb;
    uint16_t last_seq;
    uint32_t roc;
    uint32_t srtcp_index;
    int ready;
};

/*
 * RFC 3711 §4.3 的密钥派生（KDF）：用 master key 对 master salt 做 AES-CM 加密，
 * 把 label 异或进 salt 得到每类密钥的派生 IV。
 *
 * 为什么用 AES-128-ECB + 手工计数：KDF 本质是把 IV 当作数据做一次 AES 加密，
 * 再把加密块当 IV 自增（大端进位）继续——和 ICM 的计数器推进是同一种机器。
 * 输入和输出可能不等长，故按 16 字节块逐步推进、尾部截断。
 */
static void srtp_kdf(const uint8_t *master_key, const uint8_t *master_salt, uint8_t label,
                     uint8_t *out, size_t out_len)
{
    EVP_CIPHER_CTX *ctx;
    uint8_t iv[16];
    uint8_t block[16];
    size_t off = 0;
    int n;
    memset(iv, 0, sizeof(iv));
    memcpy(iv, master_salt, ZMS_WEBRTC_SRTP_SALT_LEN);
    iv[7] ^= label;
    ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        return;
    }
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, master_key, NULL) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        return;
    }
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    while (off < out_len) {
        if (EVP_EncryptUpdate(ctx, block, &n, iv, 16) != 1) {
            break;
        }
        memcpy(out + off, block, out_len - off >= 16 ? 16 : out_len - off);
        off += out_len - off >= 16 ? 16 : out_len - off;
        for (int i = 15; i >= 0; --i) {
            if (++iv[i] != 0) {
                break;
            }
        }
    }
    EVP_CIPHER_CTX_free(ctx);
}

/*
 * 维护 ROC（rollover counter）。RTP 的 seq 只有 16 位，回绕时靠 ROC 补高位，
 * 拼成 48 位的包序号 index。
 *
 * 判回绕：seq 比上次小、且差值大于 0x8000（半个序号空间），说明不是“乱序
 * 小回退”而是真的跨过了 65535→0。用 0x8000 而不是 1 是因为乱序包很常见，
 * 小幅度倒退只是重传/乱序，不能误判成新的 epoch。
 *
 * 首次调用（!ready）只记录起点、ROC 归零，避免上来就误判回绕。
 */
static void srtp_seq_update(struct zms_webrtc_srtp *s, uint16_t seq)
{
    if (!s->ready) {
        s->last_seq = seq;
        s->roc = 0;
        s->ready = 1;
        return;
    }
    if (seq < s->last_seq && s->last_seq - seq > 0x8000) {
        ++s->roc;
    }
    s->last_seq = seq;
}

static uint32_t srtp_index(const struct zms_webrtc_srtp *s, uint16_t seq)
{
    return (s->roc << 16) | seq;
}

static int srtp_aes_ecb_init_key(struct zms_webrtc_srtp *s, const uint8_t *key)
{
    if (!s || !key) {
        return -1;
    }
    if (!s->ecb) {
        s->ecb = EVP_CIPHER_CTX_new();
        if (!s->ecb) {
            return -1;
        }
    }
    if (EVP_EncryptInit_ex(s->ecb, EVP_aes_128_ecb(), NULL, key, NULL) != 1) {
        return -1;
    }
    EVP_CIPHER_CTX_set_padding(s->ecb, 0);
    return 0;
}

static int srtp_aes_ecb_init(struct zms_webrtc_srtp *s)
{
    return srtp_aes_ecb_init_key(s, s ? s->enc_key : NULL);
}

/*
 * AES-ICM 加解密（SRTP 的流密码）。加密与解密是同一操作（XOR keystream）。
 *
 * IV 布局（RFC 3711 §4.1.1）：SSRC(4) | ROC(4) | SEQ(2) | BLOCK(2)，
 * 再与 session_salt 前 12 字节异或。前 12 字节（SSRC/ROC/SEQ）固定，只有
 * 末 2 字节的块计数在每条报文内递增——这样同一报文的不同 16 字节块用不同
 * keystream，同一 seq 重放也不会复用。
 */
static int srtp_aes_icm_crypt(struct zms_webrtc_srtp *s, uint32_t ssrc, uint32_t index,
                              uint8_t *data, size_t len)
{
    uint8_t counter[16];
    uint8_t keystream[16];
    size_t i, pos = 0;
    int out_len = 0;
    uint32_t roc = index >> 16;
    uint16_t seq = (uint16_t)(index & 0xffffu);
    if (!s || !data || len == 0) {
        return -1;
    }
    if (srtp_aes_ecb_init(s) != 0) {
        return -1;
    }
    memset(counter, 0, sizeof(counter));
    counter[4] = (uint8_t)(ssrc >> 24);
    counter[5] = (uint8_t)(ssrc >> 16);
    counter[6] = (uint8_t)(ssrc >> 8);
    counter[7] = (uint8_t)(ssrc);
    counter[8] = (uint8_t)(roc >> 24);
    counter[9] = (uint8_t)(roc >> 16);
    counter[10] = (uint8_t)(roc >> 8);
    counter[11] = (uint8_t)(roc);
    counter[12] = (uint8_t)(seq >> 8);
    counter[13] = (uint8_t)(seq);
    for (i = 0; i < ZMS_WEBRTC_SRTP_SALT_LEN; ++i) {
        counter[i] ^= s->session_salt[i];
    }
    while (pos < len) {
        uint16_t block_index = (uint16_t)(pos >> 4);
        counter[14] = (uint8_t)(block_index >> 8);
        counter[15] = (uint8_t)(block_index);
        if (EVP_EncryptUpdate(s->ecb, keystream, &out_len, counter, 16) != 1 || out_len != 16) {
            return -1;
        }
        for (i = 0; i < 16 && pos < len; ++i, ++pos) {
            data[pos] ^= keystream[i];
        }
    }
    return 0;
}

/*
 * 从 DTLS-SRTP 协商出的 master key/salt 派生全部 6 类密钥（RFC 3711 §4.3.2）。
 * label 0/1/2 = SRTP 加密/认证/salt，3/4/5 = SRTCP 加密/认证/salt。
 * SRTCP 用独立密钥是为了让控制与媒体密钥隔离：泄露/重放一侧不牵连另一侧。
 * 发送和接收共用同一套派生（init_recv 直接复用），因为 DTLS 协商的是一对
 * 对称主密钥，方向区分由各端各自的 key/salt 承担。
 */
int zms_webrtc_srtp_init_send(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    if (!ctx || !key || !salt) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    srtp_kdf(key, salt, 0x00, ctx->enc_key, sizeof(ctx->enc_key));
    srtp_kdf(key, salt, 0x01, ctx->auth_key, sizeof(ctx->auth_key));
    srtp_kdf(key, salt, 0x02, ctx->session_salt, sizeof(ctx->session_salt));
    srtp_kdf(key, salt, 0x03, ctx->rtcp_enc_key, sizeof(ctx->rtcp_enc_key));
    srtp_kdf(key, salt, 0x04, ctx->rtcp_auth_key, sizeof(ctx->rtcp_auth_key));
    srtp_kdf(key, salt, 0x05, ctx->rtcp_session_salt, sizeof(ctx->rtcp_session_salt));
    return srtp_aes_ecb_init(ctx);
}

zms_webrtc_srtp *zms_webrtc_srtp_create(void)
{
    return (zms_webrtc_srtp *)calloc(1, sizeof(zms_webrtc_srtp));
}

void zms_webrtc_srtp_destroy(zms_webrtc_srtp *ctx)
{
    if (!ctx) {
        return;
    }
    if (ctx->ecb) {
        EVP_CIPHER_CTX_free(ctx->ecb);
        ctx->ecb = NULL;
    }
    free(ctx);
}

void zms_webrtc_srtp_bind_scratch(zms_webrtc_srtp *ctx, uint8_t *buf, size_t cap)
{
    if (!ctx || !buf || cap < ZMS_WEBRTC_PLAY_CRYPT_BYTES) {
        return;
    }
    ctx->auth_send_buf = buf;
    ctx->auth_recv_buf = buf;
    ctx->auth_buf_cap = cap;
}
/* RFC 3550 头 + CSRC + 扩展：SRTP 仅加密/认证其后字节。 */
static size_t srtp_plain_rtp_prefix_len(const uint8_t *rtp, size_t len)
{
    size_t off = zms_rtp_payload_offset(rtp, len);
    return off > 0 ? off : 0;
}

static void srtp_diag(const char *step, size_t len, size_t hdr_len, size_t body_len,
                      const struct zms_webrtc_srtp *ctx)
{
#if defined(ZMS_WEBRTC_SRTP_DIAG)
    static unsigned n;
    if (n >= 24u) {
        return;
    }
    ++n;
    zn_info("[webrtc] srtp_diag tid=%llu step=%s len=%zu hdr=%zu body=%zu ctx=%p ecb=%p ready=%d",
             (unsigned long long)zb_thread_self_id(), step ? step : "?", len, hdr_len, body_len,
             (const void *)ctx, ctx ? (void *)ctx->ecb : NULL, ctx ? ctx->ready : -1);
#else
    (void)step;
    (void)len;
    (void)hdr_len;
    (void)body_len;
    (void)ctx;
#endif
}
/* OpenSSL HMAC 写出完整摘要（SHA-1 为 20 字节）；SRTP 仅用前 10 字节。 */
static int srtp_hmac_tag(const uint8_t *auth_key, size_t auth_key_len, const uint8_t *data,
                         size_t data_len, uint8_t out_tag[ZMS_WEBRTC_SRTP_TAG_LEN])
{
    unsigned int digest_len = EVP_MAX_MD_SIZE;
    uint8_t digest[EVP_MAX_MD_SIZE];
    if (!auth_key || !data || !out_tag) {
        return -1;
    }
    if (!HMAC(EVP_sha1(), auth_key, (int)auth_key_len, data, data_len, digest, &digest_len) ||
        digest_len < ZMS_WEBRTC_SRTP_TAG_LEN) {
        return -1;
    }
    memcpy(out_tag, digest, ZMS_WEBRTC_SRTP_TAG_LEN);
    return 0;
}

/*
 * SRTP 保护报文（发送路径）：只加密 payload，头部保持明文（路由器/中间盒
 * 需要读 RTP 头），再对整个包 + 4 字节显式 ROC 做 HMAC-SHA1 认证。
 *
 * 认证输入末尾追加 ROC 不是冗余：HMAC 覆盖的明文里若不带上 ROC，接收端就
 * 无法对跨 RC 边界的包做一致性校验，回绕后的包可能被错误接受。
 *
 * cap 是调用方缓冲上限：加密不改变长度，但追加 10 字节 tag 会，故先检查。
 */
static int srtp_protect_impl(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap)
{
    uint16_t seq;
    uint32_t ssrc;
    uint32_t index;
    size_t hdr_len;
    size_t body_len;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    uint32_t roc;
    if (!ctx || !rtp || !len_io || *len_io < 12) {
        return -1;
    }
    if (*len_io + ZMS_WEBRTC_SRTP_TAG_LEN > cap) {
        return -1;
    }
    hdr_len = srtp_plain_rtp_prefix_len(rtp, *len_io);
    if (hdr_len == 0 || hdr_len > *len_io || hdr_len < 12) {
        return -1;
    }
    body_len = *len_io - hdr_len;
    srtp_diag("hdr_ok", *len_io, hdr_len, body_len, ctx);
    if (body_len > 0 && (hdr_len + body_len > cap || hdr_len + body_len > *len_io)) {
        return -1;
    }
    seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);
    ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) | ((uint32_t)rtp[10] << 8) | rtp[11];
    srtp_seq_update(ctx, seq);
    index = srtp_index(ctx, seq);
    roc = index >> 16;
    if (body_len > 0) {
        srtp_diag("icm_in", *len_io, hdr_len, body_len, ctx);
        if (srtp_aes_icm_crypt(ctx, ssrc, index, rtp + hdr_len, body_len) != 0) {
            return -1;
        }
        srtp_diag("icm_ok", *len_io, hdr_len, body_len, ctx);
    }
    if (*len_io + 4 > ctx->auth_buf_cap || !ctx->auth_send_buf) {
        return -1;
    }
    memcpy(ctx->auth_send_buf, rtp, *len_io);
    ctx->auth_send_buf[*len_io + 0] = (uint8_t)(roc >> 24);
    ctx->auth_send_buf[*len_io + 1] = (uint8_t)(roc >> 16);
    ctx->auth_send_buf[*len_io + 2] = (uint8_t)(roc >> 8);
    ctx->auth_send_buf[*len_io + 3] = (uint8_t)(roc);
    srtp_diag("hmac_in", *len_io, hdr_len, body_len, ctx);
    if (srtp_hmac_tag(ctx->auth_key, sizeof(ctx->auth_key), ctx->auth_send_buf, *len_io + 4, tag) !=
        0) {
        return -1;
    }
    srtp_diag("hmac_ok", *len_io, hdr_len, body_len, ctx);
    memcpy(rtp + *len_io, tag, ZMS_WEBRTC_SRTP_TAG_LEN);
    *len_io += ZMS_WEBRTC_SRTP_TAG_LEN;
    srtp_diag("done", *len_io, hdr_len, body_len, ctx);
    return 0;
}

/*
 * 发送保护的对外入口。
 *
 * Windows 上用 SEH 兜底：SRTP 加密在 poller IO 回调里跑，一旦越界访问会直接
 * 崩掉整个进程；这里把访问违例转成 -1 并打诊断日志，让上层按“该包发送失败”
 * 处理而不是静默丢失整条连接。属于最后一道防线，不替代正常的长度校验。
 */
int zms_webrtc_srtp_protect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap)
{
#if defined(_WIN32)
    __try {
        return srtp_protect_impl(ctx, rtp, len_io, cap);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        zn_warn("[webrtc] FAULT srtp_protect code=0x%08x ctx=%p len=%zu cap=%zu",
                 (unsigned)GetExceptionCode(), (void *)ctx, len_io ? *len_io : 0u, cap);
        return -1;
    }
#else
    return srtp_protect_impl(ctx, rtp, len_io, cap);
#endif
}

int zms_webrtc_srtp_init_recv(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    return zms_webrtc_srtp_init_send(ctx, key, salt);
}

/*
 * SRTP 解保护（接收路径）。步骤顺序是关键：**先认证、后解密**。
 * 认证失败直接丢弃，绝不把未认证的密文解密后交给上层——否则等于给了
 * 攻击者一个“解密 oracle”。因此 HMAC 校验通过后才做 ICM 解密。
 */
int zms_webrtc_srtp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io)
{
    uint16_t seq;
    uint32_t ssrc;
    uint32_t index;
    size_t hdr_len;
    size_t body_len;
    size_t pkt_len;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    uint8_t recv_tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    uint32_t roc;
    if (!ctx || !rtp || !len_io || *len_io < 12 + ZMS_WEBRTC_SRTP_TAG_LEN) {
        return -1;
    }
    pkt_len = *len_io - ZMS_WEBRTC_SRTP_TAG_LEN;
    hdr_len = srtp_plain_rtp_prefix_len(rtp, pkt_len);
    if (hdr_len == 0) {
        return -1;
    }
    body_len = pkt_len - hdr_len;
    seq = (uint16_t)(((uint16_t)rtp[2] << 8) | rtp[3]);
    ssrc = ((uint32_t)rtp[8] << 24) | ((uint32_t)rtp[9] << 16) | ((uint32_t)rtp[10] << 8) | rtp[11];
    srtp_seq_update(ctx, seq);
    index = srtp_index(ctx, seq);
    roc = index >> 16;
    if (!ctx->auth_recv_buf || ctx->auth_buf_cap < pkt_len + 4) {
        return -1;
    }
    /* 接收暂存区可能与报文同一块内存（会话 io_buf）；先保存 tag，避免写 ROC 时覆盖。 */
    memcpy(recv_tag, rtp + pkt_len, ZMS_WEBRTC_SRTP_TAG_LEN);
    memcpy(ctx->auth_recv_buf, rtp, pkt_len);
    ctx->auth_recv_buf[pkt_len + 0] = (uint8_t)(roc >> 24);
    ctx->auth_recv_buf[pkt_len + 1] = (uint8_t)(roc >> 16);
    ctx->auth_recv_buf[pkt_len + 2] = (uint8_t)(roc >> 8);
    ctx->auth_recv_buf[pkt_len + 3] = (uint8_t)(roc);
    if (srtp_hmac_tag(ctx->auth_key, sizeof(ctx->auth_key), ctx->auth_recv_buf, pkt_len + 4, tag) !=
            0 ||
        memcmp(tag, recv_tag, ZMS_WEBRTC_SRTP_TAG_LEN) != 0) {
        return -1;
    }
    if (body_len > 0 && srtp_aes_icm_crypt(ctx, ssrc, index, rtp + hdr_len, body_len) != 0) {
        return -1;
    }
    *len_io = pkt_len;
    return 0;
}

static int srtcp_aes_icm_crypt(struct zms_webrtc_srtp *s, uint32_t ssrc, uint32_t srtcp_index,
                               uint8_t *data, size_t len)
{
    uint8_t counter[16];
    uint8_t keystream[16];
    size_t i, pos = 0;
    int out_len = 0;
    uint64_t iv;
    if (!s || !data || len == 0) {
        return -1;
    }
    if (srtp_aes_ecb_init_key(s, s->rtcp_enc_key) != 0) {
        return -1;
    }
    memset(counter, 0, sizeof(counter));
    counter[4] = (uint8_t)(ssrc >> 24);
    counter[5] = (uint8_t)(ssrc >> 16);
    counter[6] = (uint8_t)(ssrc >> 8);
    counter[7] = (uint8_t)(ssrc);
    /* SRTCP IV：SSRC || (index << 16 | block counter)（低 48bit）。 */
/* SRTCP 的 IV 与 SRTP 不同：SSRC || (index<<16 | block)，没有 ROC/SEQ 分离。
 * index 是 31 位的 SRTCP 包计数（高位置 E 标志位），故这里只取低 48 位拼接。 */
    iv = (uint64_t)srtcp_index << 16;
    counter[10] = (uint8_t)(iv >> 40);
    counter[11] = (uint8_t)(iv >> 32);
    counter[12] = (uint8_t)(iv >> 24);
    counter[13] = (uint8_t)(iv >> 16);
    counter[14] = (uint8_t)(iv >> 8);
    counter[15] = (uint8_t)(iv);
    for (i = 0; i < ZMS_WEBRTC_SRTP_SALT_LEN; ++i) {
        counter[i] ^= s->rtcp_session_salt[i];
    }
    while (pos < len) {
        uint16_t block_index = (uint16_t)(pos >> 4);
        counter[14] = (uint8_t)(block_index >> 8);
        counter[15] = (uint8_t)(block_index);
        if (EVP_EncryptUpdate(s->ecb, keystream, &out_len, counter, 16) != 1 || out_len != 16) {
            return -1;
        }
        for (i = 0; i < 16 && pos < len; ++i, ++pos) {
            data[pos] ^= keystream[i];
        }
    }
    return 0;
}
/** RFC 3711 SRTCP：明文 RTCP + 4 字节 index + 10 字节 auth tag。 */
/* 与 SRTP 的差别：SRTCP 的 RTCP 头 8 字节保持明文（含 SSRC），只加密其后；
 * index 高 bit 是 E（加密）标志，接收端凭它判断 payload 是否被加密过。 */
int zms_webrtc_srtcp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io)
{
    size_t total;
    size_t plain_len;
    size_t enc_len;
    uint32_t ssrc;
    uint32_t index_word;
    uint32_t srtcp_index;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    if (!ctx || !rtcp || !len_io || *len_io < 8 + 4 + ZMS_WEBRTC_SRTP_TAG_LEN) {
        return -1;
    }
    total = *len_io;
    plain_len = total - 4 - ZMS_WEBRTC_SRTP_TAG_LEN;
    if (plain_len < 8) {
        return -1;
    }
    enc_len = plain_len - 8;
    index_word = ((uint32_t)rtcp[plain_len] << 24) | ((uint32_t)rtcp[plain_len + 1] << 16) |
                 ((uint32_t)rtcp[plain_len + 2] << 8) | rtcp[plain_len + 3];
    srtcp_index = index_word & 0x7fffffffu;
    ssrc =
        ((uint32_t)rtcp[4] << 24) | ((uint32_t)rtcp[5] << 16) | ((uint32_t)rtcp[6] << 8) | rtcp[7];
    if (srtp_hmac_tag(ctx->rtcp_auth_key, sizeof(ctx->rtcp_auth_key), rtcp, plain_len + 4, tag) !=
            0 ||
        memcmp(tag, rtcp + plain_len + 4, ZMS_WEBRTC_SRTP_TAG_LEN) != 0) {
        return -1;
    }
    if (enc_len > 0 && srtcp_aes_icm_crypt(ctx, ssrc, srtcp_index, rtcp + 8, enc_len) != 0) {
        return -1;
    }
    *len_io = plain_len;
    return 0;
}

int zms_webrtc_srtcp_protect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io, size_t cap)
{
    size_t plain_len;
    size_t enc_len;
    uint32_t ssrc;
    uint32_t index_word;
    uint8_t tag[ZMS_WEBRTC_SRTP_TAG_LEN];
    if (!ctx || !rtcp || !len_io || *len_io < 8) {
        return -1;
    }
    plain_len = *len_io;
    if (plain_len + 4 + ZMS_WEBRTC_SRTP_TAG_LEN > cap) {
        return -1;
    }
    enc_len = plain_len - 8;
    ssrc =
        ((uint32_t)rtcp[4] << 24) | ((uint32_t)rtcp[5] << 16) | ((uint32_t)rtcp[6] << 8) | rtcp[7];
    ++ctx->srtcp_index;
    index_word = 0x80000000u | (ctx->srtcp_index & 0x7fffffffu);
    if (enc_len > 0 && srtcp_aes_icm_crypt(ctx, ssrc, ctx->srtcp_index, rtcp + 8, enc_len) != 0) {
        return -1;
    }
    rtcp[plain_len + 0] = (uint8_t)(index_word >> 24);
    rtcp[plain_len + 1] = (uint8_t)(index_word >> 16);
    rtcp[plain_len + 2] = (uint8_t)(index_word >> 8);
    rtcp[plain_len + 3] = (uint8_t)(index_word);
    if (srtp_hmac_tag(ctx->rtcp_auth_key, sizeof(ctx->rtcp_auth_key), rtcp, plain_len + 4, tag) !=
        0) {
        return -1;
    }
    memcpy(rtcp + plain_len + 4, tag, ZMS_WEBRTC_SRTP_TAG_LEN);
    *len_io = plain_len + 4 + ZMS_WEBRTC_SRTP_TAG_LEN;
    return 0;
}
#else  /* !ZMS_HAVE_WEBRTC_DTLS */
struct zms_webrtc_srtp {
    int dummy;
};

zms_webrtc_srtp *zms_webrtc_srtp_create(void)
{
    return NULL;
}

void zms_webrtc_srtp_destroy(zms_webrtc_srtp *ctx)
{
    free(ctx);
}

void zms_webrtc_srtp_bind_scratch(zms_webrtc_srtp *ctx, uint8_t *buf, size_t cap)
{
    (void)ctx;
    (void)buf;
    (void)cap;
}

int zms_webrtc_srtp_init_send(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    (void)ctx;
    (void)key;
    (void)salt;
    return -1;
}

int zms_webrtc_srtp_protect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io, size_t cap)
{
    (void)ctx;
    (void)rtp;
    (void)len_io;
    (void)cap;
    return -1;
}

int zms_webrtc_srtp_init_recv(zms_webrtc_srtp *ctx, const uint8_t *key, const uint8_t *salt)
{
    (void)ctx;
    (void)key;
    (void)salt;
    return -1;
}

int zms_webrtc_srtp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtp, size_t *len_io)
{
    (void)ctx;
    (void)rtp;
    (void)len_io;
    return -1;
}

int zms_webrtc_srtcp_unprotect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io)
{
    (void)ctx;
    (void)rtcp;
    (void)len_io;
    return -1;
}

int zms_webrtc_srtcp_protect(zms_webrtc_srtp *ctx, uint8_t *rtcp, size_t *len_io, size_t cap)
{
    (void)ctx;
    (void)rtcp;
    (void)len_io;
    (void)cap;
    return -1;
}
#endif /* ZMS_HAVE_WEBRTC_DTLS */
