/**
 * @file xml.h
 * @brief 协议消息体用的极简 XML 层：前缀无关查找、文本/属性提取、实体解码。
 *
 * 不做 DOM/XSD 校验，只服务固定消息集（ONVIF SOAP、GB28181 MANSCDP）。
 * 与 zcs/message/msg.h 配合：zcs_msg 负责消息外壳，本文件负责 body 的 XML 视图。
 */
#ifndef ZCS_MESSAGE_XML_H
#define ZCS_MESSAGE_XML_H

#include "zcs/message/msg.h"
#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 从 from 开始查找本地名 local_name 的元素，返回开标签之后的位置；无则 NULL。 */
ZCS_API const char *zcs_xml_find(const char *from, const char *local_name);
/** 同 zcs_xml_find，但在 [from, end) 范围内查找；end 为 NULL 表示不限。 */
ZCS_API const char *zcs_xml_find_between(const char *from, const char *end,
                                         const char *local_name);
/** 返回开标签 '<' 的位置（便于取属性/子元素名）。 */
ZCS_API const char *zcs_xml_find_tag_start(const char *from, const char *end,
                                           const char *local_name);
/** 取元素文本（去空白、解码基本实体）；成功返回 0。 */
ZCS_API int zcs_xml_text(const char *from, const char *local_name, char *out, size_t cap);
/** 同 zcs_xml_text，但在 [from, end) 范围内查找。成功返回 0。 */
ZCS_API int zcs_xml_text_between(const char *from, const char *end, const char *local_name,
                                 char *out, size_t cap);
/** 取元素开标签内的属性值；成功返回 0。 */
ZCS_API int zcs_xml_attr(const char *from, const char *local_name, const char *attr, char *out,
                         size_t cap);

/** 解析 tag_start 处元素的本地名（忽略命名空间前缀）。 */
ZCS_API int zcs_xml_local_name(const char *tag_start, char *out, size_t cap);

/** 取 parent 元素的第一个子元素本地名（如 SOAP Body 内的 Action）。 */
ZCS_API int zcs_xml_first_child_name(const char *xml, const char *parent, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

/**
 * 从消息 body 里取一个元素的文本（按 body_len 界定，不要求 body 以 NUL 结尾）。
 * @return 0 找到；-1 未找到/无 body/超长
 */
ZCS_API int zcs_msg_xml_get(const zcs_msg *m, const char *local_name, char *out, size_t cap);
/** 取整数文本；未找到或非法时写 def 并返回 -1。 */
ZCS_API int zcs_msg_xml_get_int(const zcs_msg *m, const char *local_name, int def, int *out);
/** 取第一个命中的根元素名（如 Notify/Response/Query/Control）。 */
ZCS_API int zcs_msg_xml_root(const zcs_msg *m, const char *const *roots, int root_n, char *out,
                             size_t cap);

#endif /* ZCS_MESSAGE_XML_H */
