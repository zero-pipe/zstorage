/**
 * @file media_json.h
 * @brief webapi JSON 输出。
 */
#ifndef ZCS_OPS_API_JSON_MEDIA_JSON_H
#define ZCS_OPS_API_JSON_MEDIA_JSON_H

#include "zcs/core/stream/hub.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zcs_json_buf {
    char *buf;
    size_t cap;
    size_t len;
} zcs_json_buf;

int zcs_json_buf_append(zcs_json_buf *jb, const char *fmt, ...);
int zcs_media_source_is_online(const zcs_media_source *s);
/** 列表 JSON（不含外层数组）；ports 可为 NULL（使用 zcs_media_events_server_ports） */
int zcs_json_append_media_item(zcs_json_buf *jb, zcs_media_source *s, int *first,
                               const zcs_media_server_ports *ports);
/** getMediaInfo：{"code":0, ...fields} */
int zcs_json_write_media_info(zcs_json_buf *jb, zcs_media_source *s,
                              const zcs_media_server_ports *ports);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_OPS_API_JSON_MEDIA_JSON_H */
