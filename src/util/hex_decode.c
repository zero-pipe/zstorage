/**
 * @file hex_decode.c
 * @brief 十六进制解码。
 */
#include "zcs/util/hex_decode.h"
#include <ctype.h>

zn_err_t zcs_hex_decode(const char *hex, uint8_t *out, size_t cap, size_t *out_len)
{
    if (!hex || !out) {
        return ZN_ERR_INVALID;
    }
    size_t pos = 0;
    int hi = -1;
    for (const char *p = hex; *p; ++p) {
        if (isspace((unsigned char)*p)) {
            continue;
        }
        int v;
        if (*p >= '0' && *p <= '9') {
            v = *p - '0';
        } else if (*p >= 'a' && *p <= 'f') {
            v = *p - 'a' + 10;
        } else if (*p >= 'A' && *p <= 'F') {
            v = *p - 'A' + 10;
        } else {
            continue;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (pos >= cap) {
                return ZN_ERR_BUFFER_TOO_SMALL;
            }
            out[pos++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (out_len) {
        *out_len = pos;
    }
    return ZN_OK;
}
