/**
 * @file test_base64.c
 * @brief Base64 编解码：RFC 4648 标准向量与边界。
 *
 * 回归背景：该算法此前在 6 个模块各有一份实现，边界处理不一致。
 * 统一后需保证：标准向量正确、无填充容忍、非法字符不越界、
 * 缓冲不足时明确报错而不是静默截断。
 */
#include "zcs/util/base64.h"

#include "zcs_test.h"


static int encode_expect(const char *in, const char *expect)
{
    char out[128];
    size_t n = 0;

    CHECK(zcs_base64_encode((const uint8_t *)in, strlen(in), out, sizeof(out), &n) == ZN_OK);
    if (strcmp(out, expect) != 0) {
        fprintf(stderr, "encode(%s) = %s, expect %s\n", in, out, expect);
        return 1;
    }
    CHECK(n == strlen(expect));
    return 0;
}

static int decode_expect(const char *in, const char *expect, size_t expect_len)
{
    uint8_t out[128];
    size_t n = 0;

    CHECK(zcs_base64_decode(in, out, sizeof(out), &n) == ZN_OK);
    CHECK(n == expect_len);
    CHECK(memcmp(out, expect, expect_len) == 0);
    return 0;
}

int main(void)
{
    uint8_t bin[8] = {0x00, 0xff, 0x10, 0x80, 0x7f, 0x01, 0x02, 0x03};
    char enc[64];
    uint8_t dec[64];
    size_t n = 0;
    size_t i;

    /* 1) RFC 4648 标准向量 */
    CHECK(encode_expect("", "") == 0);
    CHECK(encode_expect("f", "Zg==") == 0);
    CHECK(encode_expect("fo", "Zm8=") == 0);
    CHECK(encode_expect("foo", "Zm9v") == 0);
    CHECK(encode_expect("foob", "Zm9vYg==") == 0);
    CHECK(encode_expect("fooba", "Zm9vYmE=") == 0);
    CHECK(encode_expect("foobar", "Zm9vYmFy") == 0);

    /* 2) 解码（含 '=' 填充） */
    CHECK(decode_expect("Zg==", "f", 1) == 0);
    CHECK(decode_expect("Zm8=", "fo", 2) == 0);
    CHECK(decode_expect("Zm9v", "foo", 3) == 0);
    CHECK(decode_expect("Zm9vYmFy", "foobar", 6) == 0);

    /* 3) 无填充的截断输入也应可解（部分设备如此发送） */
    CHECK(decode_expect("Zg", "f", 1) == 0);
    CHECK(decode_expect("Zm8", "fo", 2) == 0);

    /* 4) 空白/换行容忍（SDP 里常见折行） */
    CHECK(decode_expect("Zm9v\r\nYmFy", "foobar", 6) == 0);
    CHECK(decode_expect(" Zm9v\tYmFy ", "foobar", 6) == 0);

    /* 5) 非法字符：停在非法处，已解出的部分有效 */
    CHECK(decode_expect("Zm9v!!", "foo", 3) == 0);

    /* 6) 二进制往返 */
    CHECK(zcs_base64_encode(bin, sizeof(bin), enc, sizeof(enc), &n) == ZN_OK);
    memset(dec, 0, sizeof(dec));
    CHECK(zcs_base64_decode(enc, dec, sizeof(dec), &n) == ZN_OK);
    CHECK(n == sizeof(bin));
    for (i = 0; i < sizeof(bin); ++i) {
        CHECK(dec[i] == bin[i]);
    }

    /* 7) 缓冲不足：必须报错而不是静默截断 */
    {
        char tiny[8];
        size_t small = 0;

        CHECK(zcs_base64_encode(bin, sizeof(bin), tiny, sizeof(tiny), &small) ==
              ZN_ERR_BUFFER_TOO_SMALL);
        CHECK(zcs_base64_decode("Zm9vYmFy", dec, 2, &small) == ZN_ERR_BUFFER_TOO_SMALL);
        CHECK(small == 2); /* 已写入的字节数仍然有效 */
    }

    /* 8) 尺寸估算函数与真实输出一致（分配缓冲用） */
    for (i = 0; i <= 12; ++i) {
        uint8_t raw[12] = {0};
        char buf[32];
        size_t len = 0;

        CHECK(zcs_base64_encoded_max(i) >= (i + 2) / 3 * 4);
        CHECK(zcs_base64_encode(raw, i, buf, sizeof(buf), &len) == ZN_OK);
        CHECK(len <= zcs_base64_encoded_max(i));
        CHECK(zcs_base64_decoded_max(len) >= i);
    }

    printf("test_base64: OK\n");
    return 0;
}
