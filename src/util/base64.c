/**
 * @file base64.c
 * @brief Base64 编解码（RFC 4648 标准字母表）。
 */
#include "zms/util/base64.h"

static const char k_b64_tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/** 单字符解码；非法字符返回 -1。 */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

size_t zms_base64_encoded_max(size_t in_len)
{
    return (in_len + 2u) / 3u * 4u;
}

size_t zms_base64_decoded_max(size_t in_len)
{
    return in_len / 4u * 3u + 3u;
}

zn_err_t zms_base64_encode(const uint8_t *in, size_t in_len, char *out, size_t cap, size_t *out_len)
{
    size_t i = 0;
    size_t o = 0;

    if (!out || cap == 0 || (!in && in_len > 0)) {
        return ZN_ERR_INVALID;
    }
    while (i + 3 <= in_len) {
        uint32_t v;

        if (o + 4 >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[o++] = k_b64_tbl[(v >> 18) & 0x3f];
        out[o++] = k_b64_tbl[(v >> 12) & 0x3f];
        out[o++] = k_b64_tbl[(v >> 6) & 0x3f];
        out[o++] = k_b64_tbl[v & 0x3f];
        i += 3;
    }
    if (i < in_len) {
        size_t rest = in_len - i;
        uint32_t v = (uint32_t)in[i] << 16;

        if (o + 4 >= cap) {
            return ZN_ERR_BUFFER_TOO_SMALL;
        }
        if (rest == 2) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        out[o++] = k_b64_tbl[(v >> 18) & 0x3f];
        out[o++] = k_b64_tbl[(v >> 12) & 0x3f];
        out[o++] = (rest == 2) ? k_b64_tbl[(v >> 6) & 0x3f] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    if (out_len) {
        *out_len = o;
    }
    return ZN_OK;
}

zn_err_t zms_base64_decode(const char *in, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t o = 0;
    int val[4];
    int n = 0;
    int truncated = 0;

    if (!in || !out) {
        return ZN_ERR_INVALID;
    }
    while (*in) {
        int v;
        uint8_t tmp[3];
        size_t k;

        if (*in == '=' || *in == ' ' || *in == '\r' || *in == '\n' || *in == '\t') {
            ++in;
            continue;
        }
        v = b64_val(*in++);
        if (v < 0) {
            break; /* 非法字符：按已有输入结束解码 */
        }
        val[n++] = v;
        if (n < 4) {
            continue;
        }
        tmp[0] = (uint8_t)((val[0] << 2) | (val[1] >> 4));
        tmp[1] = (uint8_t)(((val[1] & 0x0f) << 4) | (val[2] >> 2));
        tmp[2] = (uint8_t)(((val[2] & 0x03) << 6) | val[3]);
        for (k = 0; k < 3; ++k) {
            if (o >= cap) {
                truncated = 1;
                break;
            }
            out[o++] = tmp[k];
        }
        n = 0;
        if (truncated) {
            break;
        }
    }
    /* 尾部不足 4 字符（无 '=' 填充的截断编码） */
    if (!truncated && n >= 2) {
        uint8_t tmp[2];
        size_t k;
        size_t nbytes = (n >= 3) ? 2u : 1u;

        tmp[0] = (uint8_t)((val[0] << 2) | (val[1] >> 4));
        if (nbytes == 2) {
            tmp[1] = (uint8_t)(((val[1] & 0x0f) << 4) | (val[2] >> 2));
        }
        for (k = 0; k < nbytes; ++k) {
            if (o >= cap) {
                truncated = 1;
                break;
            }
            out[o++] = tmp[k];
        }
    }
    if (out_len) {
        *out_len = o;
    }
    return truncated ? ZN_ERR_BUFFER_TOO_SMALL : ZN_OK;
}
