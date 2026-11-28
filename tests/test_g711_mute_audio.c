/**
 * @file test_g711_mute_audio.c
 * @brief G.711 入站不得触发「补静音 AAC」。
 *
 * 回归点：zcs_live_ingest_g711_ensure 曾漏调 zcs_live_ingest_mark_real_audio，
 * 导致有真实 G.711 音轨时仍按视频节奏往 GOP 队列注入 1024 采样静音 AAC，
 * 混进 G.711 流后表现为每 1024 采样一次杂波（听感为持续爆音）。
 *
 * 验证方式：最小视频帧 + G.711 音频帧交替输入，统计队列内音频帧数量与
 * 每帧大小的分布；不得出现 1024 以下的 AAC 帧。
 */
#include "zcs/core/stream/hub.h"
#include "zcs/media/codec/codec_id.h"
#include "zcs/publish/ingest/common/stream_ingest.h"
#include "zcs/core/gop/queue.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

/* 最小 H.264 Annex-B：SPS + PPS + 一个 IDR 分片（内容不必可解码，只需形状正确）。 */
static const uint8_t k_h264_idr[] = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x0d, 0xd9, 0x00, 0x50, 0x00,
    0x1e, 0xd0, 0x0f, 0x12, 0x26, 0xa0, 0x00, 0x00, 0x00, 0x01, 0x68, 0xce,
    0x3c, 0x80, 0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, 0x10, 0xff,
    0xfe, 0xf6, 0xf0, 0x6a, 0x13, 0x2c, 0x38, 0x80,
};

int main(void)
{
    zcs_protocol_opts opts;
    zcs_live_ingest *ch;
    zcs_media_source *src;
    uint8_t g711[160];
    int i;
    int audio_frames = 0;
    int aac_sized = 0;
    int last_audio_size = -1;
    void *rec;

    memset(&opts, 0, sizeof(opts));
    opts.add_mute_audio = 1; /* 打开补静音：正是回归条件 */

    ch = zcs_live_ingest_create("g711mute", "s1", &opts);
    CHECK(ch != NULL);
    src = zcs_live_ingest_source(ch);
    CHECK(src != NULL);

    /* 1) 先来视频，制造「等待真实音轨」窗口 */
    for (i = 0; i < 3; ++i) {
        CHECK(zcs_live_ingest_input_h264_annexb(ch, k_h264_idr, sizeof(k_h264_idr),
                                                1000u * (uint32_t)i, 1000u * (uint32_t)i,
                                                i == 0) == ZN_OK);
    }

    /* 2) 再持续输入 G.711U 20 帧，同时穿插视频，跨越原来的 500ms 补静音延迟 */
    for (i = 0; i < 20; ++i) {
        memset(g711, (uint8_t)(i & 0xff), sizeof(g711));
        CHECK(zcs_live_ingest_input_g711_es(ch, ZCS_CODEC_G711U, g711, sizeof(g711),
                                            3000u + 20u * (uint32_t)i) == ZN_OK);
        CHECK(zcs_live_ingest_input_h264_annexb(ch, k_h264_idr, sizeof(k_h264_idr),
                                                3000u + 40u * (uint32_t)i,
                                                3000u + 40u * (uint32_t)i, 0) == ZN_OK);
    }

    /* 3) 音频帧若混入 AAC 静音补帧，其长度必然是 ADTS 帧量级（这里取 >=256 判定） */
    {
        size_t pending = zcs_gop_queue_pending_count(src->gop_queue);
        zcs_gop_reader *rd;
        zcs_gop_slot slot;

        CHECK(pending > 0);
        rd = zcs_gop_reader_attach_beginning(src->gop_queue);
        CHECK(rd != NULL);
        while (zcs_gop_reader_read(rd, &slot) > 0) {
            if (slot.track == ZCS_TRACK_AUDIO) {
                ++audio_frames;
                if (slot.codec == ZCS_CODEC_AAC || slot.len >= 256) {
                    ++aac_sized;
                }
                last_audio_size = (int)slot.len;
            }
        }
        zcs_gop_reader_detach(rd);
    }
    (void)rec;
    (void)last_audio_size;

    CHECK(audio_frames > 0);          /* 真实 G.711 帧应当在队列里 */
    CHECK(audio_frames <= 20);        /* 不得多出补静音 AAC 帧 */
    CHECK(aac_sized == 0);            /* 不得出现 1024 长度的 AAC 补帧 */

    zcs_live_ingest_destroy(ch);
    printf("test_g711_mute_audio: OK (audio=%d)\n", audio_frames);
    return 0;
}
