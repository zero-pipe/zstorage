/**
 * @file zs_slot.c
 * @brief 预分配槽位：创建 / 追加 / 索引 / 封口 / 崩溃恢复。
 */
#include "storage/zs_slot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#define ZS_FSEEK _fseeki64
#define ZS_FTELL _ftelli64
#else
#include <unistd.h>
#define ZS_FSEEK fseeko
#define ZS_FTELL ftello
#endif

#include "zb/crc32.h"

#define ZS_SCAN_WIN_BYTES (128u * 1024u)
#define ZS_DATA_MIN_BYTES (64u * 1024u)

/* 头结构必须正好 512 字节，否则 Zone 布局全部错位。 */
typedef char zs_slot_header_size_check[(sizeof(zs_slot_header) == ZS_SLOT_HEADER_BYTES) ? 1 : -1];
typedef char zs_keyframe_entry_size_check[(sizeof(zs_keyframe_entry) == ZS_KEYFRAME_ENTRY_BYTES) ? 1
                                                                                               : -1];

struct zs_slot {
    FILE *fp;
    char *path;
    zs_slot_header h;
};

static uint32_t zs_header_crc(const zs_slot_header *src)
{
    zs_slot_header t = *src;

    t.header_crc = 0;
    return (uint32_t)crc32(0, (const unsigned char *)&t, (unsigned int)sizeof(t));
}

static int zs_header_write(zs_slot *s)
{
    zs_slot_header t = s->h;

    t.header_crc = zs_header_crc(&s->h);
    if (ZS_FSEEK(s->fp, 0, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(&t, 1, sizeof(t), s->fp) != sizeof(t)) {
        return -1;
    }
    return fflush(s->fp) == 0 ? 0 : -1;
}

static int zs_index_read(const zs_slot *s, uint32_t idx, zs_keyframe_entry *out)
{
    uint64_t off = (uint64_t)ZS_SLOT_ZONE_A_BYTES + (uint64_t)idx * ZS_KEYFRAME_ENTRY_BYTES;

    if (!s || !out || idx >= s->h.index_capacity) {
        return -1;
    }
    if (ZS_FSEEK(s->fp, (long long)off, SEEK_SET) != 0) {
        return -1;
    }
    if (fread(out, 1, sizeof(*out), s->fp) != sizeof(*out)) {
        return -1;
    }
    return 0;
}

static int zs_index_write(zs_slot *s, uint32_t idx, const zs_keyframe_entry *e)
{
    uint64_t off = (uint64_t)ZS_SLOT_ZONE_A_BYTES + (uint64_t)idx * ZS_KEYFRAME_ENTRY_BYTES;

    if (ZS_FSEEK(s->fp, (long long)off, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(e, 1, sizeof(*e), s->fp) != sizeof(*e)) {
        return -1;
    }
    return 0;
}

long zs_ps_pack_len(const uint8_t *b, size_t avail)
{
    size_t stuffing;
    size_t off;
    int units = 0;

    if (!b || avail < 14) {
        return -1;
    }
    if (b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xBA) {
        return -1;
    }
    stuffing = (size_t)(b[13] & 0x07u);
    off = 14u + stuffing;
    if (avail < off + 6u) {
        return -1;
    }
    /* 一个 PS pack 可以包含多个 PES/系统头，直到下一个 pack 头为止。
     * libmpeg 把一帧打成一个 pack，大帧会在同一 pack 内切多个 PES。
     * 遇到不是 PES 起的字节（例如预分配区里的 0）即视为本 pack 结束，
     * 这样"最后一个 pack 后面跟着未写入区域"也能正确判定长度。 */
    for (;;) {
        size_t len;

        if (off == avail) {
            break; /* 缓冲区正好在 pack 末尾结束 */
        }
        if (avail < off + 6u) {
            return -1; /* 尾部残包 */
        }
        if (b[off] == 0 && b[off + 1] == 0 && b[off + 2] == 1 && b[off + 3] == 0xBA) {
            break; /* 下一个 pack 开始，当前 pack 到此结束 */
        }
        if (b[off] != 0 || b[off + 1] != 0 || b[off + 2] != 1 || b[off + 3] < 0xBB ||
            b[off + 3] == 0xFF) {
            break; /* 不是 PES 起头：pack 数据到此为止 */
        }
        len = ((size_t)b[off + 4] << 8) | (size_t)b[off + 5];
        /* PES 长度为 0 表示无界，无法定位 pack 末尾 -> 视为不可判定 */
        if (len < 3u || avail < off + 6u + len) {
            return -1;
        }
        off += 6u + len;
        units++;
    }
    /* 只有 pack 头没有任何 PES：说明 pack 头后面其实是未写入区域（例如断电时
     * 只写了 10 字节转过头），不能算作一个完整 pack。 */
    return units > 0 ? (long)off : -1;
}

static size_t zs_find_ps_hdr(const uint8_t *b, size_t n, size_t from)
{
    size_t i;

    if (n < 4) {
        return (size_t)-1;
    }
    for (i = from; i + 4 <= n; ++i) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1 && b[i + 3] == 0xBA) {
            return i;
        }
    }
    return (size_t)-1;
}

/**
 * 从 start 起扫描 PS pack 链，返回最后一个完整 pack 的结束偏移。
 * 遇到脏数据时向后找下一个 00 00 01 BA 重新同步（跳过的字节不计入返回值）。
 */
static uint64_t zs_scan_ps_end(FILE *fp, uint64_t start, uint64_t end, uint32_t *resyncs)
{
    uint8_t *buf;
    uint64_t pos = start;
    uint64_t good = start;

    buf = (uint8_t *)malloc(ZS_SCAN_WIN_BYTES);
    if (!buf) {
        return start;
    }
    while (pos < end) {
        uint64_t remain = end - pos;
        size_t want = (size_t)(remain < (uint64_t)ZS_SCAN_WIN_BYTES ? remain : ZS_SCAN_WIN_BYTES);
        size_t n;
        size_t off = 0;

        if (ZS_FSEEK(fp, (long long)pos, SEEK_SET) != 0) {
            break;
        }
        n = fread(buf, 1, want, fp);
        if (n < 4) {
            break;
        }
        for (;;) {
            long pl;

            if (off + 4 > n) {
                break;
            }
            pl = zs_ps_pack_len(buf + off, n - off);
            if (pl <= 0 || (size_t)pl > n - off) {
                break;
            }
            off += (size_t)pl;
        }
        if (off == 0) {
            size_t k = zs_find_ps_hdr(buf, n, 1);

            if (k == (size_t)-1) {
                if (remain <= (uint64_t)n) {
                    break; /* 尾部只剩垃圾 */
                }
                pos += (uint64_t)n; /* 整窗无包头，跳过（good 不动） */
                continue;
            }
            off = k;
            if (resyncs) {
                (*resyncs)++;
            }
        }
        good = pos + (uint64_t)off;
        pos = good;
    }
    free(buf);
    return good;
}

static int zs_file_truncate(FILE *fp, uint64_t size)
{
    if (fflush(fp) != 0) {
        return -1;
    }
#if defined(_WIN32) || defined(_WIN64)
    return _chsize_s(_fileno(fp), (long long)size) == 0 ? 0 : -1;
#else
    return ftruncate(fileno(fp), (off_t)size) == 0 ? 0 : -1;
#endif
}

/* 按数据区终点裁剪索引，返回有效条数；同时刷新段起止时间。 */
static uint32_t zs_index_rebuild(zs_slot *s, uint64_t write_off)
{
    zs_keyframe_entry e;
    zs_keyframe_entry prev;
    uint32_t cap = s->h.index_capacity;
    uint32_t i;
    uint32_t n = 0;
    int have_prev = 0;

    for (i = 0; i < cap; ++i) {
        if (zs_index_read(s, i, &e) != 0) {
            break;
        }
        if (e.file_offset < s->h.data_start_off || e.file_offset >= write_off) {
            break;
        }
        if (have_prev && e.utc_ms < prev.utc_ms) {
            break;
        }
        prev = e;
        have_prev = 1;
        n++;
    }
    s->h.keyframe_count = n;
    if (n > 0) {
        zs_keyframe_entry first;
        zs_keyframe_entry last;

        if (zs_index_read(s, 0, &first) == 0 && zs_index_read(s, n - 1, &last) == 0) {
            s->h.seg_start_utc_ms = first.utc_ms;
            s->h.seg_end_utc_ms = last.utc_ms;
        }
    }
    return n;
}

static void zs_index_crc_update(zs_slot *s)
{
    uint32_t crc = 0;
    uint32_t i;
    zs_keyframe_entry e;

    for (i = 0; i < s->h.keyframe_count; ++i) {
        if (zs_index_read(s, i, &e) != 0) {
            break;
        }
        crc = (uint32_t)crc32(crc, (const unsigned char *)&e, (unsigned int)sizeof(e));
    }
    s->h.index_crc = crc;
}

static void zs_slot_set_stream(zs_slot_header *h, const char *stream)
{
    memset(h->stream, 0, sizeof(h->stream));
    if (stream && stream[0]) {
        strncpy(h->stream, stream, sizeof(h->stream) - 1u);
    }
}

/** 头的流标识必须自洽：不足 MAX 时必须以 0 结尾。 */
static int zs_stream_valid(const char *stream)
{
    size_t i;

    for (i = 0; i < ZS_SLOT_STREAM_MAX; ++i) {
        if (stream[i] == '\0') {
            return 1;
        }
    }
    return 0;
}

static int zs_header_valid(const zs_slot_header *h)
{
    uint32_t crc;

    if (h->magic != ZS_SLOT_MAGIC || h->version != ZS_SLOT_VERSION) {
        return 0;
    }
    crc = zs_header_crc(h);
    if (crc != h->header_crc) {
        return 0;
    }
    if (h->index_bytes < ZS_KEYFRAME_ENTRY_BYTES) {
        return 0;
    }
    if (h->data_start_off != (uint64_t)ZS_SLOT_ZONE_A_BYTES + h->index_bytes) {
        return 0;
    }
    if (h->data_end_off != h->file_bytes || h->file_bytes <= h->data_start_off) {
        return 0;
    }
    if (h->data_write_off < h->data_start_off || h->data_write_off > h->data_end_off) {
        return 0;
    }
    if (h->keyframe_count > h->index_capacity) {
        return 0;
    }
    if (!zs_stream_valid(h->stream)) {
        return 0;
    }
    return 1;
}

zs_slot *zs_slot_create(const char *path, const zs_slot_opts *opts)
{
    zs_slot *s;
    uint32_t index_bytes;
    uint64_t data_bytes;

    if (!path || !opts) {
        return NULL;
    }
    index_bytes = opts->index_bytes ? opts->index_bytes : ZS_SLOT_DEFAULT_INDEX_BYTES;
    index_bytes -= index_bytes % ZS_KEYFRAME_ENTRY_BYTES;
    data_bytes = opts->slot_bytes > (uint64_t)ZS_SLOT_ZONE_A_BYTES + index_bytes
                     ? opts->slot_bytes - (uint64_t)ZS_SLOT_ZONE_A_BYTES - index_bytes
                     : 0;
    if (data_bytes < ZS_DATA_MIN_BYTES) {
        return NULL;
    }

    s = (zs_slot *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->path = (char *)malloc(strlen(path) + 1);
    if (!s->path) {
        free(s);
        return NULL;
    }
    strcpy(s->path, path);
    s->fp = fopen(path, "wb+");
    if (!s->fp) {
        free(s->path);
        free(s);
        return NULL;
    }

    memset(&s->h, 0, sizeof(s->h));
    s->h.magic = ZS_SLOT_MAGIC;
    s->h.version = ZS_SLOT_VERSION;
    s->h.state = ZS_SLOT_WRITING;
    s->h.channel_id = opts->channel_id;
    s->h.slot_seq = opts->slot_seq;
    zs_slot_set_stream(&s->h, opts->stream);
    s->h.file_bytes = opts->slot_bytes;
    s->h.index_bytes = index_bytes;
    s->h.index_capacity = index_bytes / ZS_KEYFRAME_ENTRY_BYTES;
    s->h.data_start_off = (uint64_t)ZS_SLOT_ZONE_A_BYTES + index_bytes;
    s->h.data_end_off = opts->slot_bytes;
    s->h.data_write_off = s->h.data_start_off;

    /* 预分配：把文件撑到目标大小，避免运行期因空间不足半途失败 */
    if (ZS_FSEEK(s->fp, (long long)(opts->slot_bytes - 1), SEEK_SET) != 0) {
        zs_slot_close(s);
        return NULL;
    }
    if (fputc(0, s->fp) == EOF) {
        zs_slot_close(s);
        return NULL;
    }
    if (zs_header_write(s) != 0) {
        zs_slot_close(s);
        return NULL;
    }
    return s;
}

zs_slot *zs_slot_open(const char *path)
{
    zs_slot *s;

    if (!path) {
        return NULL;
    }
    s = (zs_slot *)calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->path = (char *)malloc(strlen(path) + 1);
    if (!s->path) {
        free(s);
        return NULL;
    }
    strcpy(s->path, path);
    s->fp = fopen(path, "rb+");
    if (!s->fp) {
        free(s->path);
        free(s);
        return NULL;
    }
    if (fread(&s->h, 1, sizeof(s->h), s->fp) != sizeof(s->h) || !zs_header_valid(&s->h)) {
        zs_slot_close(s);
        return NULL;
    }
    return s;
}

void zs_slot_close(zs_slot *s)
{
    if (!s) {
        return;
    }
    if (s->fp) {
        if (s->h.state == ZS_SLOT_WRITING) {
            (void)zs_header_write(s);
        }
        fclose(s->fp);
    }
    free(s->path);
    free(s);
}

int zs_slot_recover(zs_slot *s, int seal_after)
{
    uint64_t write_off;
    uint32_t resyncs = 0;

    if (!s || !s->fp) {
        return -1;
    }
    write_off = zs_scan_ps_end(s->fp, s->h.data_start_off, s->h.data_end_off, &resyncs);
    if (write_off > s->h.data_end_off) {
        write_off = s->h.data_end_off;
    }
    s->h.data_write_off = write_off;
    /* 索引按数据区终点裁剪（末尾半帧对应的关键帧会被丢弃） */
    (void)zs_index_rebuild(s, write_off);
    zs_index_crc_update(s);
    if (seal_after) {
        s->h.state = ZS_SLOT_SEALED;
    }
    return zs_header_write(s);
}

int zs_slot_append(zs_slot *s, const void *buf, size_t len)
{
    if (!s) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    if (!s->fp || !buf) {
        return -1;
    }
    if (s->h.state != ZS_SLOT_WRITING) {
        return -1;
    }
    if (s->h.data_write_off + len > s->h.data_end_off) {
        return -1; /* 调用方应先封口并切换槽位 */
    }
    if (ZS_FSEEK(s->fp, (long long)s->h.data_write_off, SEEK_SET) != 0) {
        return -1;
    }
    if (fwrite(buf, 1, len, s->fp) != len) {
        return -1;
    }
    s->h.data_write_off += len;
    return 0;
}

int zs_slot_add_keyframe(zs_slot *s, uint64_t utc_ms, int64_t pts_90k, uint64_t file_offset,
                         uint32_t event_mask)
{
    zs_keyframe_entry e;

    if (!s || s->h.state != ZS_SLOT_WRITING) {
        return -1;
    }
    if (s->h.keyframe_count >= s->h.index_capacity) {
        return -1;
    }
    if (file_offset < s->h.data_start_off || file_offset >= s->h.data_end_off) {
        return -1;
    }
    memset(&e, 0, sizeof(e));
    e.utc_ms = utc_ms;
    e.pts_90k = (uint64_t)pts_90k;
    e.file_offset = file_offset;
    e.event_mask = event_mask;
    if (zs_index_write(s, s->h.keyframe_count, &e) != 0) {
        return -1;
    }
    s->h.keyframe_count++;
    if (s->h.keyframe_count == 1) {
        s->h.seg_start_utc_ms = utc_ms;
    }
    s->h.seg_end_utc_ms = utc_ms;
    return 0;
}

int zs_slot_seal(zs_slot *s)
{
    if (!s || !s->fp) {
        return -1;
    }
    if (fflush(s->fp) != 0) {
        return -1;
    }
    zs_index_crc_update(s);
    s->h.state = ZS_SLOT_SEALED;
    return zs_header_write(s);
}

int zs_slot_reset(zs_slot *s)
{
    if (!s || !s->fp) {
        return -1;
    }
    if (fflush(s->fp) != 0) {
        return -1;
    }
    s->h.state = ZS_SLOT_FREE;
    s->h.channel_id = 0;
    memset(s->h.stream, 0, sizeof(s->h.stream));
    s->h.keyframe_count = 0;
    s->h.index_crc = 0;
    s->h.data_write_off = s->h.data_start_off;
    s->h.seg_start_utc_ms = 0;
    s->h.seg_end_utc_ms = 0;
    return zs_header_write(s);
}

int zs_slot_begin(zs_slot *s, uint32_t channel_id, const char *stream)
{
    if (!s || !s->fp) {
        return -1;
    }
    if (s->h.state != ZS_SLOT_FREE && s->h.state != ZS_SLOT_WRITING) {
        return -1;
    }
    s->h.state = ZS_SLOT_WRITING;
    s->h.channel_id = channel_id;
    zs_slot_set_stream(&s->h, stream);
    s->h.keyframe_count = 0;
    s->h.index_crc = 0;
    s->h.data_write_off = s->h.data_start_off;
    s->h.seg_start_utc_ms = 0;
    s->h.seg_end_utc_ms = 0;
    return zs_header_write(s);
}

int zs_slot_flush(zs_slot *s)
{
    if (!s || !s->fp) {
        return -1;
    }
    if (fflush(s->fp) != 0) {
        return -1;
    }
    if (s->h.state == ZS_SLOT_WRITING) {
        return zs_header_write(s);
    }
    return 0;
}

int zs_slot_find(const zs_slot *s, uint64_t utc_ms, zs_keyframe_entry *out)
{
    uint32_t lo = 0;
    uint32_t hi;
    zs_keyframe_entry mid_e;
    uint32_t best = 0;
    int found = 0;

    if (!s || !out || s->h.keyframe_count == 0) {
        return 1;
    }
    hi = s->h.keyframe_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;

        if (zs_index_read(s, mid, &mid_e) != 0) {
            return -1;
        }
        if (mid_e.utc_ms <= utc_ms) {
            best = mid;
            found = 1;
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (!found) {
        return 1;
    }
    return zs_index_read(s, best, out) == 0 ? 0 : -1;
}

int zs_slot_get_keyframe(const zs_slot *s, uint32_t idx, zs_keyframe_entry *out)
{
    if (!s || !out || idx >= s->h.keyframe_count) {
        return -1;
    }
    return zs_index_read(s, idx, out);
}

int zs_slot_pread(const zs_slot *s, void *buf, size_t len, uint64_t off)
{
    if (!s || !s->fp || !buf) {
        return -1;
    }
    if (off < s->h.data_start_off || off + len > s->h.data_end_off) {
        return -1;
    }
    if (ZS_FSEEK(s->fp, (long long)off, SEEK_SET) != 0) {
        return -1;
    }
    return fread(buf, 1, len, s->fp) == len ? 0 : -1;
}

uint64_t zs_slot_write_offset(const zs_slot *s)
{
    return s ? s->h.data_write_off : 0;
}

uint64_t zs_slot_data_start(const zs_slot *s)
{
    return s ? s->h.data_start_off : 0;
}

uint64_t zs_slot_data_end(const zs_slot *s)
{
    return s ? s->h.data_end_off : 0;
}

uint64_t zs_slot_file_bytes(const zs_slot *s)
{
    return s ? s->h.file_bytes : 0;
}

uint64_t zs_slot_segment_start_ms(const zs_slot *s)
{
    return s ? s->h.seg_start_utc_ms : 0;
}

uint64_t zs_slot_segment_end_ms(const zs_slot *s)
{
    return s ? s->h.seg_end_utc_ms : 0;
}

uint32_t zs_slot_keyframe_count(const zs_slot *s)
{
    return s ? s->h.keyframe_count : 0;
}

uint32_t zs_slot_index_capacity(const zs_slot *s)
{
    return s ? s->h.index_capacity : 0;
}

uint32_t zs_slot_state(const zs_slot *s)
{
    return s ? s->h.state : 0;
}

uint32_t zs_slot_channel_id(const zs_slot *s)
{
    return s ? s->h.channel_id : 0;
}

const char *zs_slot_stream(const zs_slot *s)
{
    return s ? s->h.stream : "";
}

uint64_t zs_slot_remaining(const zs_slot *s)
{
    return s ? s->h.data_end_off - s->h.data_write_off : 0;
}
