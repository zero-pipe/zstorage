/**
 * @file test_zbase.c
 * @brief zbase：容器/编码/解析/时间基础库回归。
 */
#include "zb/base64.h"
#include "zb/bitmap.h"
#include "zb/bits.h"
#include "zb/darray.h"
#include "zb/hashmap.h"
#include "zb/heap.h"
#include "zb/jhash.h"
#include "zb/md5.h"
#include "zb/ntp-time.h"
#include "zb/rbtree.h"
#include "zb/sha.h"
#include "zb/time64.h"
#include "zb/uri-parse.h"
#include "zb/cstringext.h"
#include "zb/urlcodec.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int test_darray(void)
{
    struct darray_t a;
    int v;

    memset(&a, 0, sizeof(a));
    darray_init(&a, sizeof(int), 4);
    for (v = 0; v < 10; ++v) {
        CHECK(darray_push_back(&a, &v) == 0);
    }
    CHECK(darray_count(&a) == 10);
    CHECK(*(int *)darray_get(&a, 9) == 9);
    v = 100;
    CHECK(darray_insert(&a, 3, &v) == 0);
    CHECK(*(int *)darray_get(&a, 3) == 100);
    CHECK(darray_erase(&a, 3) == 0);
    CHECK(*(int *)darray_get(&a, 3) == 3);
    darray_free(&a);
    return 0;
}

static int test_hashmap(void)
{
    struct hashmap_t m;
    int i;

    CHECK(hashmap_init(&m, 8, sizeof(int)) == 0);
    for (i = 0; i < 200; ++i) {
        int key = i;
        int val = i * 3;
        unsigned int h = jhash(&key, sizeof(key), 0x1234);
        CHECK(hashmap_insert(&m, h, &key, (void *)(size_t)val) >= 0);
    }
    for (i = 0; i < 200; ++i) {
        int key = i;
        unsigned int h = jhash(&key, sizeof(key), 0x1234);
        CHECK((size_t)hashmap_get(&m, h, &key) == (size_t)(i * 3));
    }
    for (i = 0; i < 200; i += 2) {
        int key = i;
        unsigned int h = jhash(&key, sizeof(key), 0x1234);
        CHECK(hashmap_delete(&m, h, &key) == 0);
    }
    for (i = 0; i < 200; ++i) {
        int key = i;
        unsigned int h = jhash(&key, sizeof(key), 0x1234);
        if ((i & 1) == 0) {
            CHECK(hashmap_get(&m, h, &key) == NULL);
        } else {
            const void *got = hashmap_get(&m, h, &key);
            if ((size_t)got != (size_t)(i * 3)) {
                fprintf(stderr, "hashmap miss i=%d key=%d got=%p want=%d\n", i, key, got,
                        i * 3);
                return 1;
            }
        }
    }
    hashmap_clear(&m);
    return 0;
}

struct rb_item {
    struct rbtree_node_t node;
    int key;
};

static int rb_insert(struct rbtree_root_t *root, struct rb_item *item)
{
    struct rbtree_node_t **link = &root->node;
    struct rbtree_node_t *parent = NULL;

    while (*link) {
        struct rb_item *cur = (struct rb_item *)((char *)*link -
                                                 offsetof(struct rb_item, node));
        parent = *link;
        if (item->key < cur->key) {
            link = &(*link)->left;
        } else if (item->key > cur->key) {
            link = &(*link)->right;
        } else {
            return -1;
        }
    }
    rbtree_insert(root, parent, link, &item->node);
    return 0;
}

static int test_rbtree(void)
{
    struct rbtree_root_t root;
    struct rb_item items[32];
    const struct rbtree_node_t *n;
    int i;
    int expect = 0;

    memset(&root, 0, sizeof(root));
    memset(items, 0, sizeof(items));
    for (i = 0; i < 32; ++i) {
        items[i].key = (i * 17) % 32;
        CHECK(rb_insert(&root, &items[i]) == 0);
    }
    for (n = rbtree_first(&root); n; n = rbtree_next(n)) {
        const struct rb_item *it = (const struct rb_item *)((const char *)n -
                                                            offsetof(struct rb_item, node));
        CHECK(it->key == expect);
        ++expect;
    }
    CHECK(expect == 32);
    for (i = 0; i < 32; i += 2) {
        rbtree_delete(&root, &items[i].node);
    }
    expect = 1;
    for (n = rbtree_first(&root); n; n = rbtree_next(n)) {
        const struct rb_item *it = (const struct rb_item *)((const char *)n -
                                                            offsetof(struct rb_item, node));
        CHECK(it->key == expect);
        expect += 2;
    }
    CHECK(expect == 33);
    return 0;
}

static int int_less(void *param, const void *a, const void *b)
{
    (void)param;
    return *(const int *)a < *(const int *)b;
}

static int test_heap(void)
{
    heap_t *h = heap_create(int_less, NULL);
    int vals[] = {5, 1, 9, 3, 7, 2, 8, 6, 4, 0};
    int i;
    int last = -1;

    CHECK(h != NULL);
    for (i = 0; i < 10; ++i) {
        CHECK(heap_push(h, &vals[i]) == 0);
    }
    CHECK(heap_size(h) == 10);
    while (!heap_empty(h)) {
        int v = *(int *)heap_top(h);
        CHECK(v >= last);
        last = v;
        heap_pop(h);
    }
    heap_destroy(h);
    return 0;
}

static int test_bitmap(void)
{
    uint8_t bm[2];

    memset(bm, 0, sizeof(bm));
    bitmap_zero(bm, 12);
    bitmap_set(bm, 1, 4);
    CHECK(bitmap_weight(bm, 12) == 4);
    CHECK(bitmap_test_bit(bm, 0) == 0);
    CHECK(bitmap_test_bit(bm, 1) != 0);
    CHECK(bitmap_test_bit(bm, 4) != 0);
    CHECK(bitmap_test_bit(bm, 5) == 0);
    bitmap_clear(bm, 2, 2);
    CHECK(bitmap_weight(bm, 12) == 2);
    CHECK(bitmap_test_bit(bm, 1) != 0);
    CHECK(bitmap_test_bit(bm, 4) != 0);
    CHECK(bitmap_find_first_zero(bm, 12) == 0);
    bitmap_set(bm, 0, 1);
    CHECK(bitmap_find_first_zero(bm, 12) == 2);
    return 0;
}

static int test_bits(void)
{
    uint8_t buf[2] = {0xA5, 0x5A};
    struct bits_t bits;

    bits_init(&bits, buf, sizeof(buf));
    CHECK(bits_read_n(&bits, 4) == 0xA);
    CHECK(bits_read_n(&bits, 4) == 0x5);
    CHECK(bits_next_n(&bits, 4) == 0x5);
    CHECK(bits_read_n(&bits, 8) == 0x5A);
    return 0;
}

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; ++i) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static int test_digest(void)
{
    uint8_t md5_digest[16];
    uint8_t sha1_digest[SHA1HashSize];
    uint8_t sha256_digest[SHA256HashSize];
    MD5_CTX md5;
    SHA1Context sha1;
    SHA256Context sha256;
    char hex[2 * SHA512HashSize + 1];
    const char *abc = "abc";

    MD5Init(&md5);
    MD5Update(&md5, (unsigned char *)abc, 3);
    MD5Final(md5_digest, &md5);
    to_hex(md5_digest, sizeof(md5_digest), hex);
    CHECK(strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0);

    CHECK(SHA1Reset(&sha1) == 0);
    CHECK(SHA1Input(&sha1, (const uint8_t *)abc, 3) == 0);
    CHECK(SHA1Result(&sha1, sha1_digest) == 0);
    to_hex(sha1_digest, sizeof(sha1_digest), hex);
    CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);

    CHECK(SHA256Reset(&sha256) == 0);
    CHECK(SHA256Input(&sha256, (const uint8_t *)abc, 3) == 0);
    CHECK(SHA256Result(&sha256, sha256_digest) == 0);
    to_hex(sha256_digest, sizeof(sha256_digest), hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    return 0;
}

static int test_base64(void)
{
    char enc[64];
    uint8_t dec[64];
    char hex[32];

    {
        size_t n = base64_encode(enc, "abc", 3);
        CHECK(n == 4);
        enc[n] = 0;
        CHECK(strcmp(enc, "YWJj") == 0);
    }
    CHECK(base64_decode(dec, enc, strlen(enc)) == 3);
    CHECK(memcmp(dec, "abc", 3) == 0);
    {
        size_t n = base16_encode(hex, "abc", 3);
        CHECK(n == 6);
        hex[n] = 0;
        CHECK(strcmp(hex, "616263") == 0);
    }
    CHECK(base16_decode(dec, hex, strlen(hex)) == 3);
    CHECK(memcmp(dec, "abc", 3) == 0);
    return 0;
}

static int test_uri_url(void)
{
    struct uri_t *u;
    const char *src = "http://user:pass@example.com:8080/a/b?x=1&y=2#frag";
    char enc[64];
    char dec[64];

    u = uri_parse(src, (int)strlen(src));
    CHECK(u != NULL);
    CHECK(u->scheme && strcmp(u->scheme, "http") == 0);
    CHECK(u->host && strcmp(u->host, "example.com") == 0);
    CHECK(u->port == 8080);
    CHECK(u->path && strcmp(u->path, "/a/b") == 0);
    CHECK(u->query && strcmp(u->query, "x=1&y=2") == 0);
    uri_free(u);

    CHECK(url_encode("a b&c", -1, enc, sizeof(enc)) > 0);
    CHECK(strcmp(enc, "a+b%26c") == 0);
    CHECK(url_decode(enc, -1, dec, sizeof(dec)) == 5);
    CHECK(strcmp(dec, "a b&c") == 0);
    return 0;
}

static int test_time(void)
{
    time64_t t;
    char buf[64];
    struct tm64 tm;

    t = time64_from("%Y-%M-%D %h:%m:%s", "2024-01-02 03:04:05");
    CHECK(t > 0);
    CHECK(time64_format(t, "%04Y-%02M-%02D %02h:%02m:%02s", buf) == 0);
    CHECK(strcmp(buf, "2024-01-02 03:04:05") == 0);
    CHECK(time64_utc(t, &tm) == 0);
    CHECK(tm.year == 2024 - 1900 && tm.month == 0 && tm.day == 2);
    CHECK(time64_now() > 0);
    CHECK(ntp64_now() > 0);
    return 0;
}

static int test_string(void)
{
    char buf[16];
    const char *next = NULL;
    size_t n;

    CHECK(strlcpy(buf, "abc", sizeof(buf)) == 3 && strcmp(buf, "abc") == 0);
    CHECK(strlcat(buf, "def", sizeof(buf)) == 6 && strcmp(buf, "abcdef") == 0);

    n = strsplit("ab,c, def", "ab,c, def" + 9, ",", "", &next);
    CHECK(n == 2 && memcmp("ab", "ab", 2) == 0);
    CHECK(next && strncmp(next, "c, def", 6) == 0);

    n = 5;
    CHECK(strncmp(strtrim("  abc  ", &n, " ", " "), "abc", 3) == 0 && n == 3);
    return 0;
}

int main(void)
{
    CHECK(test_darray() == 0);
    CHECK(test_hashmap() == 0);
    CHECK(test_rbtree() == 0);
    CHECK(test_heap() == 0);
    CHECK(test_bitmap() == 0);
    CHECK(test_bits() == 0);
    CHECK(test_digest() == 0);
    CHECK(test_base64() == 0);
    CHECK(test_uri_url() == 0);
    CHECK(test_time() == 0);
    CHECK(test_string() == 0);
    printf("test_zbase: ok\n");
    return 0;
}
