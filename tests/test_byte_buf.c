/**
 * @file test_byte_buf.c
 * @brief znet byte_buf 追加语义：普通追加、自引用追加（源落在本缓冲内）。
 *
 * 回归：HTTP 分帧在 remain 内“清空后回拼自身剩余字节”会走自引用追加，
 * 旧实现用 memcpy 导致区间重叠（ASan memcpy-param-overlap）；fuzz 发现。
 */
#include "zn/util/byte_buf.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

int main(void)
{
    zn_byte_buf *b = zn_byte_buf_create(4);
    const char *data;
    size_t size;

    CHECK(b != NULL);

    /* 1) 普通追加（含多次扩容） */
    CHECK(zn_byte_buf_append(b, "hello", 5) == ZN_OK);
    CHECK(zn_byte_buf_append(b, " world", 6) == ZN_OK);
    CHECK(zn_byte_buf_size(b) == 11);
    data = zn_byte_buf_data(b);
    CHECK(data && memcmp(data, "hello world", 11) == 0);
    CHECK(data[11] == '\0');

    /* 2) 自引用追加：把自身 [4,11) = "o world" 追加回末尾，结果应为
     *    "hello worldo world"。旧实现 memcpy 重叠 -> ASan 报错。 */
    {
        const char *self = zn_byte_buf_data(b) + 4;
        size_t self_len = zn_byte_buf_size(b) - 4;
        CHECK(zn_byte_buf_append(b, self, self_len) == ZN_OK);
    }
    CHECK(zn_byte_buf_size(b) == 18);
    data = zn_byte_buf_data(b);
    CHECK(memcmp(data, "hello worldo world", 18) == 0);
    CHECK(data[18] == '\0');

    /* 3) 清空后从非零偏移回拼（framer 的实际用法：clear 后 append 缓冲内某段）。
     *    注意 clear 只清零 data[0]，故源必须从 >0 的偏移开始才有意义。 */
    zn_byte_buf_clear(b);
    CHECK(zn_byte_buf_append(b, "abcdef", 6) == ZN_OK);
    {
        const char *self = zn_byte_buf_data(b) + 2; /* "cdef" */
        size_t self_len = zn_byte_buf_size(b) - 2;
        zn_byte_buf_clear(b); /* 逻辑清空，data 缓冲仍在（仅 data[0] 被置 0） */
        CHECK(zn_byte_buf_append(b, self, self_len) == ZN_OK);
    }
    CHECK(zn_byte_buf_size(b) == 4);
    CHECK(memcmp(zn_byte_buf_data(b), "cdef", 4) == 0);

    /* 4) 自引用 + 触发扩容（源偏移在 realloc 后仍要正确） */
    zn_byte_buf_clear(b);
    CHECK(zn_byte_buf_append(b, "0123456789", 10) == ZN_OK);
    {
        const char *self = zn_byte_buf_data(b) + 2; /* "23456789" */
        size_t self_len = 8;
        /* 追加后长度 18，超过初始容量，触发 realloc */
        CHECK(zn_byte_buf_append(b, self, self_len) == ZN_OK);
    }
    CHECK(zn_byte_buf_size(b) == 18);
    CHECK(memcmp(zn_byte_buf_data(b), "012345678923456789", 18) == 0);

    zn_byte_buf_destroy(b);
    printf("test_byte_buf: ok\n");
    return 0;
}
