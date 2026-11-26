#ifndef ZM_CORE_READER_H
#define ZM_CORE_READER_H

/**
 * @file reader.h
 * @brief 统一 demux 接口：每个容器格式一个实现，codec 只是 packet 里的值。
 */
#include "zm/core/io.h"
#include "zm/core/packet.h"
#include "zm/core/track.h"
#include "zm/zm_export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum zm_read_mode {
    ZM_READ_ES = 0x01,  /**< 产出 zm_packet */
    ZM_READ_RAW = 0x02, /**< 产出 zm_raw_unit（relay） */
} zm_read_mode;

typedef struct zm_reader_cfg {
    zm_format format;
    unsigned mode;
    int jitter_ms;        /**< RTP only */
    const char *encoding; /**< RTP only */
    int payload_type;     /**< RTP only */
    int input_tag_body;   /**< FLV: input 是已拆好的 tag body */
    const void *opts;     /**< 格式私有配置，可为 NULL */
} zm_reader_cfg;

typedef struct zm_reader zm_reader;

typedef struct zm_reader_ops {
    int (*input)(zm_reader *r, const void *buf, size_t len);
    int (*read)(zm_reader *r, zm_packet *out);
    int (*read_raw)(zm_reader *r, zm_raw_unit *out);
    int (*seek)(zm_reader *r, int64_t *ms);
    const zm_track *(*tracks)(zm_reader *r, size_t *count);
    void (*close)(zm_reader *r);
} zm_reader_ops;

struct zm_reader {
    const zm_reader_ops *ops;
    void *impl;
};

/* 包装函数：未实现的 ops 返回 ZM_ERR_NOT_SUPPORTED。 */
ZM_API int zm_reader_input(zm_reader *r, const void *buf, size_t len);
ZM_API int zm_reader_read(zm_reader *r, zm_packet *out);
ZM_API int zm_reader_read_raw(zm_reader *r, zm_raw_unit *out);
ZM_API int zm_reader_seek(zm_reader *r, int64_t *ms);
ZM_API const zm_track *zm_reader_tracks(zm_reader *r, size_t *count);

/** 调用实现自己的 close；实现负责释放自身内存。 */
ZM_API void zm_reader_close(zm_reader *r);

#ifdef __cplusplus
}
#endif

#endif /* ZM_CORE_READER_H */
