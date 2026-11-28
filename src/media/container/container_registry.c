/**
 * @file container_registry.c
 * @brief 容器格式注册表（demuxer + muxer vtable）。
 *
 * 两个按 zcs_container_id 索引的静态查找表。
 * 所有内建格式经 zcs_container_register_all() 注册一次，
 * 由 module_registry 在启动时调用。无惰性自初始化。
 */
#include "zcs/media/container/container_registry.h"
#include "zcs/media/wire/rtp_packet.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* 槽位上限；所有枚举值须小于此值 */
#define ZCS_CONTAINER_SLOT_MAX 16

/* 各 .c 中定义的 extern vtable 实例 */
extern const zcs_container_demuxer_ops zcs_container_rtsp_interleaved_ops;
extern const zcs_container_demuxer_ops zcs_container_flv_tag_ops;
extern const zcs_container_demuxer_ops zcs_container_flv_file_ops;
extern const zcs_container_demuxer_ops zcs_container_mp4_ops;
extern const zcs_container_demuxer_ops zcs_container_mkv_ops;
extern const zcs_container_demuxer_ops zcs_container_mpegts_demuxer_ops;
extern const zcs_container_demuxer_ops zcs_container_zslot_ops;
extern const zcs_container_muxer_ops zcs_container_mpegts_muxer_ops;

/* 按容器 ID 索引的 vtable 指针表 */
static const zcs_container_demuxer_ops *g_demux[ZCS_CONTAINER_SLOT_MAX];
static const zcs_container_muxer_ops *g_mux[ZCS_CONTAINER_SLOT_MAX];

/* 容器 ID 到可读名称（按枚举顺序） */
static const char *k_names[] = {
    "invalid", "rtsp-interleaved", "flv-tag", "mpegts", "mp4", "mkv", "flv-file",
};

/** @return 容器格式的可读名称；id 越界返回 "invalid"。 */
const char *zcs_container_name(zcs_container_id id)
{
    if ((unsigned)id >= sizeof(k_names) / sizeof(k_names[0])) {
        return "invalid";
    }
    return k_names[id];
}

/** 注册 demuxer vtable；无效或越界 id 静默忽略。 */
void zcs_container_register_demuxer(const zcs_container_demuxer_ops *ops)
{
    if (!ops || ops->id <= 0 || ops->id >= ZCS_CONTAINER_SLOT_MAX) {
        return;
    }
    g_demux[ops->id] = ops;
}

/**
 * 按容器 ID 查找 demuxer vtable。
 * 须在 zcs_container_register_all() 之后调用（module_registry 保证）。
 */
const zcs_container_demuxer_ops *zcs_container_demuxer_find(zcs_container_id id)
{
    if (id <= 0 || id >= ZCS_CONTAINER_SLOT_MAX) {
        return NULL;
    }
    return g_demux[id];
}

/** 注册 muxer vtable；无效或越界 id 静默忽略。 */
void zcs_container_register_muxer(const zcs_container_muxer_ops *ops)
{
    if (!ops || ops->id <= 0 || ops->id >= ZCS_CONTAINER_SLOT_MAX) {
        return;
    }
    g_mux[ops->id] = ops;
}

/**
 * 按容器 ID 查找 muxer vtable。
 * 须在 zcs_container_register_all() 之后调用（module_registry 保证）。
 */
const zcs_container_muxer_ops *zcs_container_muxer_find(zcs_container_id id)
{
    if (id <= 0 || id >= ZCS_CONTAINER_SLOT_MAX) {
        return NULL;
    }
    return g_mux[id];
}

/**
 * 注册所有内建容器格式（demuxer + muxer）。
 * 启动时由 zcs_modules_register_all() 调用一次；幂等。
 */
void zcs_container_register_all(void)
{
    static int registered; /* 启动阶段，单线程 */

    if (registered) {
        return;
    }
    registered = 1;
    /* Demuxer：RTSP interleaved、FLV（流/文件）、MP4、MKV、MPEG-TS、zslot */
    zcs_container_register_demuxer(&zcs_container_rtsp_interleaved_ops);
    zcs_container_register_demuxer(&zcs_container_flv_tag_ops);
    zcs_container_register_demuxer(&zcs_container_flv_file_ops);
    zcs_container_register_demuxer(&zcs_container_mp4_ops);
    zcs_container_register_demuxer(&zcs_container_mkv_ops);
    zcs_container_register_demuxer(&zcs_container_mpegts_demuxer_ops);
    zcs_container_register_demuxer(&zcs_container_zslot_ops);
    /* Muxer：MPEG-TS（HLS 分片） */
    zcs_container_register_muxer(&zcs_container_mpegts_muxer_ops);
}

zn_err_t zcs_container_rtsp_interleaved_parse(const uint8_t *data, size_t len,
                                               zcs_container_packet *out)
{
    uint8_t channel;
    const uint8_t *payload;
    size_t plen;

    if (!out) {
        return ZN_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    if (zcs_rtsp_interleaved_read(data, len, &channel, &payload, &plen) != ZN_OK) {
        return ZN_ERR_INVALID;
    }
    out->kind = ZCS_CONTAINER_PKT_RTP;
    out->channel = channel;
    out->data = payload;
    out->len = plen;
    return ZN_OK;
}
