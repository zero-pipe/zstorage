/**
 * @file zs_playback.c
 * @brief 槽位数据区导出为标准 PS 文件（供 VOD 链路回放）。
 */
#include "storage/zs_playback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZS_EXPORT_CHUNK (256u * 1024u)

int zs_playback_relative_ms(const zs_slot *s, uint64_t abs_ms, uint64_t *rel_ms)
{
    zs_keyframe_entry first;
    uint64_t base;

    if (!s || !rel_ms || zs_slot_keyframe_count(s) == 0) {
        return -1;
    }
    if (zs_slot_get_keyframe(s, 0, &first) != 0) {
        return -1;
    }
    base = first.utc_ms;
    *rel_ms = abs_ms > base ? abs_ms - base : 0;
    return 0;
}

/* 找到 keyframe_count 中第一个 utc >= ms 的下标；没有返回 count。 */
static uint32_t zs_first_kf_at_or_after(const zs_slot *s, uint64_t ms)
{
    uint32_t lo = 0;
    uint32_t hi = zs_slot_keyframe_count(s);

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        zs_keyframe_entry e;

        if (zs_slot_get_keyframe(s, mid, &e) != 0) {
            return lo;
        }
        if (e.utc_ms < ms) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return lo;
}

/* 找到最后一个 utc <= ms 的关键帧下标；没有返回 -1。 */
static int zs_last_kf_at_or_before(const zs_slot *s, uint64_t ms)
{
    uint32_t lo = 0;
    uint32_t hi = zs_slot_keyframe_count(s);
    int found = -1;

    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        zs_keyframe_entry e;

        if (zs_slot_get_keyframe(s, mid, &e) != 0) {
            break;
        }
        if (e.utc_ms <= ms) {
            found = (int)mid;
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return found;
}

/**
 * 从 src_off 顺序拷到 end_off，写入 fp。返回拷贝字节数；失败 -1。
 * 分块读，单块失败即中止（宁可少导出，也不导出拼接错误的 PS）。
 */
static int64_t zs_copy_range(const zs_slot *s, uint64_t src_off, uint64_t end_off, FILE *fp)
{
    uint8_t *buf;
    uint64_t done = 0;

    if (end_off <= src_off) {
        return 0;
    }
    buf = (uint8_t *)malloc(ZS_EXPORT_CHUNK);
    if (!buf) {
        return -1;
    }
    while (src_off + done < end_off) {
        uint64_t remain = end_off - (src_off + done);
        size_t want = (size_t)(remain < (uint64_t)ZS_EXPORT_CHUNK ? remain : ZS_EXPORT_CHUNK);

        if (zs_slot_pread(s, buf, want, src_off + done) != 0) {
            free(buf);
            return -1;
        }
        if (fwrite(buf, 1, want, fp) != want) {
            free(buf);
            return -1;
        }
        done += want;
    }
    free(buf);
    return (int64_t)done;
}

int64_t zs_playback_export_ps(const zs_slot *s, const char *out_path, uint64_t max_bytes)
{
    FILE *fp;
    uint64_t off;
    uint64_t end;
    int64_t n;

    if (!s || !out_path || !out_path[0]) {
        return -1;
    }
    off = zs_slot_data_start(s);
    end = zs_slot_write_offset(s);
    if (max_bytes > 0 && off + max_bytes < end) {
        end = off + max_bytes;
    }
    if (end <= off) {
        return -1; /* 槽位没有数据 */
    }
    fp = fopen(out_path, "wb");
    if (!fp) {
        return -1;
    }
    n = zs_copy_range(s, off, end, fp);
    if (fclose(fp) != 0) {
        return -1;
    }
    return n;
}

int64_t zs_playback_export_range(const zs_slot *s, uint64_t from_ms, uint64_t to_ms,
                                 const char *out_path, uint64_t *actual_start)
{
    FILE *fp;
    uint64_t start_off;
    uint64_t end_off;
    zs_keyframe_entry kf;
    int idx;
    int64_t n;

    if (!s || !out_path || !out_path[0] || zs_slot_keyframe_count(s) == 0) {
        return -1;
    }
    /* 起点：<= from_ms 的最近关键帧（保证可解码） */
    idx = zs_last_kf_at_or_before(s, from_ms);
    if (idx < 0) {
        idx = 0;
    }
    if (zs_slot_get_keyframe(s, (uint32_t)idx, &kf) != 0) {
        return -1;
    }
    start_off = kf.file_offset;
    if (actual_start) {
        *actual_start = kf.utc_ms;
    }

    /* 终点：> to_ms 的首个关键帧之前（不切断 GOP） */
    {
        uint32_t after = zs_first_kf_at_or_after(s, to_ms + 1u);
        zs_keyframe_entry next;

        if (after < zs_slot_keyframe_count(s) && zs_slot_get_keyframe(s, after, &next) == 0) {
            end_off = next.file_offset;
        } else {
            end_off = zs_slot_write_offset(s);
        }
    }
    if (end_off <= start_off) {
        end_off = zs_slot_write_offset(s);
    }
    if (end_off <= start_off) {
        return -1;
    }

    fp = fopen(out_path, "wb");
    if (!fp) {
        return -1;
    }
    n = zs_copy_range(s, start_off, end_off, fp);
    if (fclose(fp) != 0) {
        return -1;
    }
    return n;
}
