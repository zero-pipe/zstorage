#ifndef ZM_CORE_WRITER_H
#define ZM_CORE_WRITER_H

/**
 * @file writer.h
 * @brief 统一 mux 接口：packet -> 字节流 sink。
 */
#include "zm/core/io.h"
#include "zm/core/packet.h"
#include "zm/core/track.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zm_write_mode {
    ZM_WRITE_ES = 0x01,  /**< zm_packet -> 容器 */
    ZM_WRITE_RAW = 0x02, /**< zm_raw_unit 原样转发 */
} zm_write_mode;

typedef struct zm_writer_cfg {
    zm_format format;
    unsigned mode;
    int flags;              /**< 格式私有标志 */
    const zm_track *tracks;
    size_t track_count;
    int tag_body;           /**< FLV: sink 只要 tag body（RTMP），不要完整 tag */
    const void *opts;
} zm_writer_cfg;

typedef struct zm_writer zm_writer;

typedef struct zm_writer_ops {
    int (*write)(zm_writer *w, const zm_packet *pkt);
    int (*write_raw)(zm_writer *w, const zm_raw_unit *unit);
    int (*flush)(zm_writer *w, int force_boundary);
    int (*finish)(zm_writer *w);
    void (*close)(zm_writer *w);
} zm_writer_ops;

struct zm_writer {
    const zm_writer_ops *ops;
    void *impl;
};

ZM_API int zm_writer_write(zm_writer *w, const zm_packet *pkt);
ZM_API int zm_writer_write_raw(zm_writer *w, const zm_raw_unit *unit);
ZM_API int zm_writer_flush(zm_writer *w, int force_boundary);
ZM_API int zm_writer_finish(zm_writer *w);

/** 调用实现自己的 close；实现负责释放自身内存。 */
ZM_API void zm_writer_close(zm_writer *w);

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_WRITER_H */
