/**
 * @file amf.c
 * @brief AMF0 编解码（RTMP / FLV script tag）。
 *
 * AMF0 是二进制、大端序的格式：每种值以 1 字节类型标记开头。本文件只实现 RTMP 实际
 * 用到的子集（number/boolean/string/long-string/object/ecma-array/strict-array/null/
 * undefined），未知类型返回 NOT_IMPL 由调用方忽略。
 *
 * 安全前提：输入来自网络，所有读取都先做边界检查（*off + n > len 即失败），对端
 * 截断/伪造长度时直接报错而不是越界读。对象成员名截断到 key[64]，超长键只保留前
 * 63 字节——AMF 命令里用不到更长的键。
 *
 * 浮点字节序用运行期探针（k_endian_probe）判断，避免依赖编译期宏；大端机直接
 * memcpy，小端机逐字节反转。编码端对称处理。
 */
#include "zcs/media/wire/amf.h"
#include <stdlib.h>
#include <string.h>

struct zcs_amf_value {
    zcs_amf_type type;
    char *str;
    double num;
    int boolean;
    struct zcs_amf_pair {
        char key[64];
        zcs_amf_value *val;
    } *pairs;
    unsigned pair_count;
};

static zcs_amf_value *amf_new(zcs_amf_type t)
{
    zcs_amf_value *v = (zcs_amf_value *)calloc(1, sizeof(*v));
    if (v) {
        v->type = t;
    }
    return v;
}

void zcs_amf_value_free(zcs_amf_value *v)
{
    if (!v) {
        return;
    }
    free(v->str);
    for (unsigned i = 0; i < v->pair_count; ++i) {
        zcs_amf_value_free(v->pairs[i].val);
    }
    free(v->pairs);
    free(v);
}

static zn_err_t decode_one(const uint8_t *data, size_t len, size_t *off, zcs_amf_value **out);

/* 读一个 AMF0 短字符串：U16 大端长度 + UTF-8 字节。分配 len+1 并补 '\0'，让上层能
 * 直接当 C 字符串用（AMF 字符串本身不带终止符）。 */
static zn_err_t read_string(const uint8_t *data, size_t len, size_t *off, char **out)
{
    if (*off + 2 > len) {
        return ZN_ERR_INVALID;
    }
    uint16_t slen = (uint16_t)((data[*off] << 8) | data[*off + 1]);
    *off += 2;
    if (*off + slen > len) {
        return ZN_ERR_INVALID;
    }
    char *s = (char *)malloc((size_t)slen + 1);
    if (!s) {
        return ZN_ERR_NOMEM;
    }
    if (slen) {
        memcpy(s, data + *off, slen);
    }
    s[slen] = '\0';
    *off += slen;
    *out = s;
    return ZN_OK;
}

/*
 * 递归解码一个值。每步都先校验边界，任何越界/非法终止都释放已分配的部分并失败
 * （调用方看到非 OK 即整体丢弃，不会拿到半构造对象）。
 * 细节：
 *   - LONG_STRING(0x0c) 解码后统一标记成 STRING，上层不区分长短；
 *   - STRICT_ARRAY 只消费元素但仍逐个解码后释放，最后把值当作 NULL——命令参数里
 *     数组内容基本不用，保留类型可用于判断“这里有个数组”；
 *   - OBJECT / ECMA_ARRAY 用 key 长度 0 + 单字节 0x09(marker) 作为终止（"00 00 09"）。
 */
static zn_err_t decode_one(const uint8_t *data, size_t len, size_t *off, zcs_amf_value **out)
{
    if (*off >= len) {
        return ZN_ERR_INVALID;
    }
    uint8_t t = data[(*off)++];
    zcs_amf_value *v = amf_new((zcs_amf_type)t);
    if (!v) {
        return ZN_ERR_NOMEM;
    }

    switch (t) {
    case ZCS_AMF_NUMBER:
        if (*off + 8 > len) {
            zcs_amf_value_free(v);
            return ZN_ERR_INVALID;
        }
        {
            static const double k_endian_probe = 1.0;
            const uint8_t *p = data + *off;
            uint8_t *dst = (uint8_t *)&v->num;

            if (*(const char *)&k_endian_probe == 0) {
                dst[0] = p[7];
                dst[1] = p[6];
                dst[2] = p[5];
                dst[3] = p[4];
                dst[4] = p[3];
                dst[5] = p[2];
                dst[6] = p[1];
                dst[7] = p[0];
            } else {
                memcpy(&v->num, p, 8);
            }
        }
        *off += 8;
        break;
    case ZCS_AMF_BOOLEAN:
        if (*off + 1 > len) {
            zcs_amf_value_free(v);
            return ZN_ERR_INVALID;
        }
        v->boolean = data[(*off)++] ? 1 : 0;
        break;
    case ZCS_AMF_STRING:
        if (read_string(data, len, off, &v->str) != ZN_OK) {
            zcs_amf_value_free(v);
            return ZN_ERR_INVALID;
        }
        break;
    case ZCS_AMF_NULL:
        break;
    case 0x06: /* AMF0_UNDEFINED */
        break;
    case 0x0c: { /* AMF0_LONG_STRING */
        if (*off + 4 > len) {
            zcs_amf_value_free(v);
            return ZN_ERR_INVALID;
        }
        uint32_t slen = ((uint32_t)data[*off] << 24) | ((uint32_t)data[*off + 1] << 16) |
                        ((uint32_t)data[*off + 2] << 8) | (uint32_t)data[*off + 3];
        *off += 4;
        if (*off + slen > len) {
            zcs_amf_value_free(v);
            return ZN_ERR_INVALID;
        }
        v->str = (char *)malloc((size_t)slen + 1);
        if (!v->str) {
            zcs_amf_value_free(v);
            return ZN_ERR_NOMEM;
        }
        if (slen) {
            memcpy(v->str, data + *off, slen);
        }
        v->str[slen] = '\0';
        *off += slen;
        v->type = ZCS_AMF_STRING;
        break;
    }
    case ZCS_AMF_STRICT_ARRAY: {
        if (*off + 4 > len) {
            zcs_amf_value_free(v);
            return ZN_ERR_INVALID;
        }
        uint32_t count = ((uint32_t)data[*off] << 24) | ((uint32_t)data[*off + 1] << 16) |
                         ((uint32_t)data[*off + 2] << 8) | (uint32_t)data[*off + 3];
        *off += 4;
        for (uint32_t i = 0; i < count; ++i) {
            zcs_amf_value *elem = NULL;
            if (decode_one(data, len, off, &elem) != ZN_OK) {
                zcs_amf_value_free(v);
                return ZN_ERR_INVALID;
            }
            zcs_amf_value_free(elem);
        }
        v->type = ZCS_AMF_NULL;
        break;
    }
    case ZCS_AMF_OBJECT:
    case ZCS_AMF_ECMA_ARRAY:
        if (t == ZCS_AMF_ECMA_ARRAY) {
            if (*off + 4 > len) {
                zcs_amf_value_free(v);
                return ZN_ERR_INVALID;
            }
            *off += 4;
        }
        for (;;) {
            if (*off + 3 > len) {
                zcs_amf_value_free(v);
                return ZN_ERR_INVALID;
            }
            uint16_t klen = (uint16_t)((data[*off] << 8) | data[*off + 1]);
            *off += 2;
            if (klen == 0) {
                if (*off + 1 > len || data[*off] != 9) {
                    zcs_amf_value_free(v);
                    return ZN_ERR_INVALID;
                }
                *off += 1;
                break;
            }
            if (*off + klen > len) {
                zcs_amf_value_free(v);
                return ZN_ERR_INVALID;
            }
            char key[64];
            size_t copy = klen < sizeof(key) - 1 ? klen : sizeof(key) - 1;
            memcpy(key, data + *off, copy);
            key[copy] = '\0';
            *off += klen;
            zcs_amf_value *child = NULL;
            if (decode_one(data, len, off, &child) != ZN_OK) {
                zcs_amf_value_free(v);
                return ZN_ERR_INVALID;
            }
            v->pairs =
                (struct zcs_amf_pair *)realloc(v->pairs, (v->pair_count + 1) * sizeof(*v->pairs));
            if (!v->pairs) {
                zcs_amf_value_free(child);
                zcs_amf_value_free(v);
                return ZN_ERR_NOMEM;
            }
            strncpy(v->pairs[v->pair_count].key, key, sizeof(v->pairs[0].key) - 1);
            v->pairs[v->pair_count].val = child;
            v->pair_count++;
        }
        break;
    default:
        zcs_amf_value_free(v);
        return ZN_ERR_NOT_IMPL;
    }
    *out = v;
    return ZN_OK;
}

zn_err_t zcs_amf_decode(const uint8_t *data, size_t len, zcs_amf_value **out, size_t *consumed)
{
    size_t off = 0;
    zn_err_t err = decode_one(data, len, &off, out);
    if (err != ZN_OK) {
        return err;
    }
    if (consumed) {
        *consumed = off;
    }
    return ZN_OK;
}

zcs_amf_type zcs_amf_value_type(const zcs_amf_value *v)
{
    return v ? v->type : ZCS_AMF_NULL;
}

const char *zcs_amf_value_str(const zcs_amf_value *v)
{
    return v && v->str ? v->str : "";
}

int zcs_amf_value_bool(const zcs_amf_value *v)
{
    return v ? v->boolean : 0;
}

double zcs_amf_value_num(const zcs_amf_value *v)
{
    return v ? v->num : 0;
}

const zcs_amf_value *zcs_amf_object_get(const zcs_amf_value *obj, const char *key)
{
    if (!obj || !key) {
        return NULL;
    }
    for (unsigned i = 0; i < obj->pair_count; ++i) {
        if (strcmp(obj->pairs[i].key, key) == 0) {
            return obj->pairs[i].val;
        }
    }
    return NULL;
}

size_t zcs_amf_encode_string(uint8_t *out, size_t cap, const char *s)
{
    size_t slen = s ? strlen(s) : 0;
    if (cap < 3 + slen) {
        return 0;
    }
    out[0] = ZCS_AMF_STRING;
    out[1] = (uint8_t)((slen >> 8) & 0xff);
    out[2] = (uint8_t)(slen & 0xff);
    if (slen) {
        memcpy(out + 3, s, slen);
    }
    return 3 + slen;
}

size_t zcs_amf_encode_strict_array_numbers(uint8_t *out, size_t cap, const double *vals,
                                           size_t count)
{
    size_t pos;
    size_t i;

    if (!out || count > 0x7fffffffu) {
        return 0;
    }
    if (cap < 5 + count * 9) {
        return 0;
    }
    out[0] = ZCS_AMF_STRICT_ARRAY;
    out[1] = (uint8_t)((count >> 24) & 0xff);
    out[2] = (uint8_t)((count >> 16) & 0xff);
    out[3] = (uint8_t)((count >> 8) & 0xff);
    out[4] = (uint8_t)(count & 0xff);
    pos = 5;
    for (i = 0; i < count; ++i) {
        size_t n = zcs_amf_encode_number(out + pos, cap - pos, vals ? vals[i] : 0.0);
        if (n == 0) {
            return 0;
        }
        pos += n;
    }
    return pos;
}

/* 编码一个 number：标记 + 8 字节大端 IEEE754。小端机需逐字节反转，见文件头说明。 */
size_t zcs_amf_encode_number(uint8_t *out, size_t cap, double n)
{
    static const double k_endian_probe = 1.0;
    const uint8_t *src;

    if (cap < 9) {
        return 0;
    }
    out[0] = ZCS_AMF_NUMBER;
    src = (const uint8_t *)&n;
    if (*(const char *)&k_endian_probe == 0) {
        out[1] = src[7];
        out[2] = src[6];
        out[3] = src[5];
        out[4] = src[4];
        out[5] = src[3];
        out[6] = src[2];
        out[7] = src[1];
        out[8] = src[0];
    } else {
        memcpy(out + 1, &n, 8);
    }
    return 9;
}

size_t zcs_amf_encode_null(uint8_t *out, size_t cap)
{
    if (cap < 1) {
        return 0;
    }
    out[0] = ZCS_AMF_NULL;
    return 1;
}

size_t zcs_amf_encode_boolean(uint8_t *out, size_t cap, int value)
{
    if (cap < 2) {
        return 0;
    }
    out[0] = ZCS_AMF_BOOLEAN;
    out[1] = value ? 1 : 0;
    return 2;
}

size_t zcs_amf_encode_object_begin(uint8_t *out, size_t cap)
{
    if (cap < 1) {
        return 0;
    }
    out[0] = ZCS_AMF_OBJECT;
    return 1;
}

size_t zcs_amf_encode_ecma_array_begin(uint8_t *out, size_t cap, uint32_t count)
{
    if (cap < 5) {
        return 0;
    }
    out[0] = ZCS_AMF_ECMA_ARRAY;
    out[1] = (uint8_t)((count >> 24) & 0xff);
    out[2] = (uint8_t)((count >> 16) & 0xff);
    out[3] = (uint8_t)((count >> 8) & 0xff);
    out[4] = (uint8_t)(count & 0xff);
    return 5;
}

size_t zcs_amf_encode_object_key(uint8_t *out, size_t cap, const char *key)
{
    size_t klen = key ? strlen(key) : 0;
    if (cap < 2 + klen) {
        return 0;
    }
    out[0] = (uint8_t)((klen >> 8) & 0xff);
    out[1] = (uint8_t)(klen & 0xff);
    if (klen) {
        memcpy(out + 2, key, klen);
    }
    return 2 + klen;
}

/* 对象结束标记 "00 00 09"：空成员名 + object-end marker。与 encode_object_begin 配对。 */
size_t zcs_amf_encode_object_end(uint8_t *out, size_t cap)
{
    if (cap < 3) {
        return 0;
    }
    out[0] = 0;
    out[1] = 0;
    out[2] = 9;
    return 3;
}

/* ── 命令参数列表 ───────────────────────────────────────────────────────── */

struct zcs_amf_args {
    zcs_amf_value **v;
    int n;
    int cap;
};

/*
 * 解析一整个 AMF0 命令参数列表（按顺序连续解码直到输入耗尽）。RTMP 的
 * connect/publish/play 等命令就是“一串 AMF0 值”，这里把它们都解出来放进数组。
 * 容错：遇到解码失败或 0 进度即停止，保留已成功解析的前缀（宁少不错）；若一个都
 * 没解出则返回 NULL。元素数组从 8 起翻倍扩容，减少小命令的 realloc。
 */
zcs_amf_args *zcs_amf_args_parse(const uint8_t *data, size_t len)
{
    zcs_amf_args *a;
    size_t off = 0;

    if (!data || len == 0) {
        return NULL;
    }
    a = (zcs_amf_args *)calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    while (off < len) {
        zcs_amf_value *val = NULL;
        size_t used = 0;

        if (zcs_amf_decode(data + off, len - off, &val, &used) != ZN_OK || !val || used == 0) {
            break;
        }
        off += used;
        if (a->n == a->cap) {
            int cap = a->cap ? a->cap * 2 : 8;
            zcs_amf_value **nv =
                (zcs_amf_value **)realloc(a->v, (size_t)cap * sizeof(*nv));

            if (!nv) {
                zcs_amf_value_free(val);
                break;
            }
            a->v = nv;
            a->cap = cap;
        }
        a->v[a->n++] = val;
    }
    if (a->n == 0) {
        free(a->v);
        free(a);
        return NULL;
    }
    return a;
}

void zcs_amf_args_free(zcs_amf_args *args)
{
    if (!args) {
        return;
    }
    for (int i = 0; i < args->n; ++i) {
        zcs_amf_value_free(args->v[i]);
    }
    free(args->v);
    free(args);
}

int zcs_amf_args_count(const zcs_amf_args *args)
{
    return args ? args->n : 0;
}

const zcs_amf_value *zcs_amf_args_at(const zcs_amf_args *args, int index)
{
    if (!args || index < 0 || index >= args->n) {
        return NULL;
    }
    return args->v[index];
}

const char *zcs_amf_args_str(const zcs_amf_args *args, int index)
{
    const zcs_amf_value *v = zcs_amf_args_at(args, index);

    return (v && zcs_amf_value_type(v) == ZCS_AMF_STRING) ? zcs_amf_value_str(v) : NULL;
}

double zcs_amf_args_num(const zcs_amf_args *args, int index, double def)
{
    const zcs_amf_value *v = zcs_amf_args_at(args, index);

    return (v && zcs_amf_value_type(v) == ZCS_AMF_NUMBER) ? zcs_amf_value_num(v) : def;
}

int zcs_amf_args_bool(const zcs_amf_args *args, int index, int def)
{
    const zcs_amf_value *v = zcs_amf_args_at(args, index);

    return (v && zcs_amf_value_type(v) == ZCS_AMF_BOOLEAN) ? zcs_amf_value_bool(v) : def;
}

const char *zcs_amf_args_obj_str(const zcs_amf_args *args, int index, const char *key)
{
    const zcs_amf_value *obj = zcs_amf_args_at(args, index);
    const zcs_amf_value *v;

    if (!obj || !key) {
        return NULL;
    }
    if (zcs_amf_value_type(obj) != ZCS_AMF_OBJECT && zcs_amf_value_type(obj) != ZCS_AMF_ECMA_ARRAY) {
        return NULL;
    }
    v = zcs_amf_object_get(obj, key);
    return (v && zcs_amf_value_type(v) == ZCS_AMF_STRING) ? zcs_amf_value_str(v) : NULL;
}

double zcs_amf_args_obj_num(const zcs_amf_args *args, int index, const char *key, double def)
{
    const zcs_amf_value *obj = zcs_amf_args_at(args, index);
    const zcs_amf_value *v;

    if (!obj || !key) {
        return def;
    }
    if (zcs_amf_value_type(obj) != ZCS_AMF_OBJECT && zcs_amf_value_type(obj) != ZCS_AMF_ECMA_ARRAY) {
        return def;
    }
    v = zcs_amf_object_get(obj, key);
    return (v && zcs_amf_value_type(v) == ZCS_AMF_NUMBER) ? zcs_amf_value_num(v) : def;
}
