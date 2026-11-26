/**
 * @file amf.h
 * @brief AMF0 编解码（RTMP / FLV script tag）。
 */
#ifndef ZMS_MEDIA_WIRE_AMF_H
#define ZMS_MEDIA_WIRE_AMF_H

#include "zms/zms_export.h"
#include "zn/zn_errno.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zms_amf_type {
    ZMS_AMF_NUMBER = 0,
    ZMS_AMF_BOOLEAN = 1,
    ZMS_AMF_STRING = 2,
    ZMS_AMF_OBJECT = 3,
    ZMS_AMF_NULL = 5,
    ZMS_AMF_ECMA_ARRAY = 8,
    ZMS_AMF_STRICT_ARRAY = 10,
} zms_amf_type;

typedef struct zms_amf_value zms_amf_value;

/**
 * 从 data 解出单个 AMF0 值。
 * @param out      输出；成功时为堆分配对象，调用方负责 zms_amf_value_free
 * @param consumed 可为 NULL；成功时写入实际消耗字节数，便于顺序解析后续值
 * @return ZN_OK 成功；数据不完整/非法返回错误码，此时不写 *out
 */
ZMS_API zn_err_t zms_amf_decode(const uint8_t *data, size_t len, zms_amf_value **out,
                                 size_t *consumed);
/** 释放 decode/object_get 产生的值树；对 NULL 调用安全。 */
ZMS_API void zms_amf_value_free(zms_amf_value *v);

/** 查询值类型；v 为 NULL 时返回 ZMS_AMF_NULL。 */
ZMS_API zms_amf_type zms_amf_value_type(const zms_amf_value *v);
/** 取字符串内容；v 非字符串或为 NULL 时返回空串 ""（不返回 NULL）。 */
ZMS_API const char *zms_amf_value_str(const zms_amf_value *v);
/** 取数值；v 非数字或为 NULL 时返回 0。 */
ZMS_API double zms_amf_value_num(const zms_amf_value *v);
/** 取布尔值；v 非布尔或为 NULL 时返回 0。 */
ZMS_API int zms_amf_value_bool(const zms_amf_value *v);
/**
 * 在 object/ecma-array 中按 key 取子值。
 * @return 借用的子值（归属 obj，随 obj 一起释放）；无此键返回 NULL。
 */
ZMS_API const zms_amf_value *zms_amf_object_get(const zms_amf_value *obj, const char *key);

/**
 * 一条命令的参数列表（按到达顺序）。
 *
 * AMF 命令体形如：null, <参数1>, <参数2>, ... —— 每个 handler 原先各自手工
 * decode_arg 一串，既重复又容易与协议顺序不一致。这里统一解析成可随机访问的
 * 列表，handler 用 zms_amf_args_* 按下标/按对象键取。
 */
typedef struct zms_amf_args zms_amf_args;

/** 解析命令体；解析失败返回 NULL（部分参数坏了则保留已解析部分）。 */
ZMS_API zms_amf_args *zms_amf_args_parse(const uint8_t *data, size_t len);
/** 释放参数列表及其所有值；对 NULL 调用安全。 */
ZMS_API void zms_amf_args_free(zms_amf_args *args);
/** 参数个数（不含命令名前的 null）；args 为 NULL 时返回 0。 */
ZMS_API int zms_amf_args_count(const zms_amf_args *args);
/**
 * 按顺序取第 index 个参数。
 * @return 借用的值（归属 args，随 args 释放失效）；越界或 NULL 返回 NULL。
 */
ZMS_API const zms_amf_value *zms_amf_args_at(const zms_amf_args *args, int index);

/** 第 index 个参数若是字符串则返回其内容，否则 NULL。 */
ZMS_API const char *zms_amf_args_str(const zms_amf_args *args, int index);
/** 第 index 个参数若是数字则返回其值，否则 def。 */
ZMS_API double zms_amf_args_num(const zms_amf_args *args, int index, double def);
/** 第 index 个参数若是布尔则返回其值，否则 def。 */
ZMS_API int zms_amf_args_bool(const zms_amf_args *args, int index, int def);
/** 第 index 个参数是对象/ECMA 数组时，取其 key 字符串；否则 NULL。 */
ZMS_API const char *zms_amf_args_obj_str(const zms_amf_args *args, int index, const char *key);
/** 同上取数字；缺失返回 def。 */
ZMS_API double zms_amf_args_obj_num(const zms_amf_args *args, int index, const char *key,
                                    double def);

/**
 * 写一个 AMF0 字符串值（类型字节 + U16 长度 + 内容）。
 * @return 写入字节数；cap 不足时返回 0 且不写。
 */
ZMS_API size_t zms_amf_encode_string(uint8_t *out, size_t cap, const char *s);
/**
 * 写一个 AMF0 number（类型字节 + 8 字节大端 double）。
 * @return 成功 9；cap 不足返回 0。
 */
ZMS_API size_t zms_amf_encode_number(uint8_t *out, size_t cap, double n);
/** AMF0 严格数字数组（类型字节 + U32 计数 + N × number） */
ZMS_API size_t zms_amf_encode_strict_array_numbers(uint8_t *out, size_t cap, const double *vals,
                                                   size_t count);
/** 写 AMF0 null；@return 成功 1，cap 不足 0。 */
ZMS_API size_t zms_amf_encode_null(uint8_t *out, size_t cap);
/** 写 AMF0 boolean；@return 成功 2，cap 不足 0。 */
ZMS_API size_t zms_amf_encode_boolean(uint8_t *out, size_t cap, int value);
/** AMF0 object/ecma-array 写入辅助：先写 begin，随后 key+value，最后 object_end。 */
ZMS_API size_t zms_amf_encode_object_begin(uint8_t *out, size_t cap);
/** 写 ecma-array 头（类型字节 + U32 计数）；@return 成功 5，cap 不足 0。 */
ZMS_API size_t zms_amf_encode_ecma_array_begin(uint8_t *out, size_t cap, uint32_t count);
/** 写 object 键（U16 长度 + key，不含类型字节）；@return 写入字节数，cap 不足 0。 */
ZMS_API size_t zms_amf_encode_object_key(uint8_t *out, size_t cap, const char *key);
/** 写 object/ecma-array 结束标记（0x000009）；@return 成功 3，cap 不足 0。 */
ZMS_API size_t zms_amf_encode_object_end(uint8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_MEDIA_WIRE_AMF_H */
