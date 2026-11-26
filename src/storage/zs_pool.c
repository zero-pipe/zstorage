/**
 * @file zs_pool.c
 * @brief 槽位池：目录扫描 / 申请复用 / 保留期回收 / 时间范围检索。
 */
#include "storage/zs_pool.h"

#include "zb/os/sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <direct.h>
#include <io.h>
#define ZS_POOL_MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define ZS_POOL_MKDIR(p) mkdir((p), 0755)
#endif

#define ZS_SLOT_FILE_FMT "slot_%08u.zslot"
#define ZS_DAY_MS (86400ULL * 1000ULL)

typedef struct zs_entry {
    uint32_t id;
    uint32_t channel_id;
    uint32_t state;
    uint32_t ref_count;
    uint64_t seg_start_ms;
    uint64_t seg_end_ms;
    char stream[ZS_SLOT_STREAM_MAX];
    char path[ZS_POOL_PATH_MAX];
} zs_entry;

struct zs_pool {
    char dir[ZS_POOL_PATH_MAX];
    uint64_t capacity_bytes;
    uint32_t retention_days;
    uint64_t slot_bytes;
    uint32_t index_bytes;
    uint32_t watermark_pct;
    uint32_t max_slots;
    uint32_t next_id;

    zs_entry *entries;
    uint32_t count;
    uint32_t cap;
    /** 池可被录制线程与 webapi 查询线程同时访问，所有公开入口都持锁。 */
    zb_mutex *mu;
};

#define ZS_POOL_LOCK(p)                                                                                 do {                                                                                                   if ((p) && (p)->mu) {                                                                                  zb_mutex_lock((p)->mu);                                                                        }                                                                                              } while (0)

#define ZS_POOL_UNLOCK(p)                                                                              do {                                                                                                   if ((p) && (p)->mu) {                                                                                  zb_mutex_unlock((p)->mu);                                                                      }                                                                                              } while (0)

void zs_pool_opts_default(zs_pool_opts *opts)
{
    if (!opts) {
        return;
    }
    memset(opts, 0, sizeof(*opts));
    opts->dir = NULL;
    opts->capacity_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL; /* 2TB */
    opts->retention_days = 7;
    opts->slot_bytes = 256ULL * 1024ULL * 1024ULL; /* 256MB */
    opts->index_bytes = ZS_SLOT_DEFAULT_INDEX_BYTES;
    opts->free_watermark_pct = 5;
}

static int zs_pool_path(char *out, size_t cap, const char *dir, uint32_t id)
{
    int n = snprintf(out, cap, "%s/" ZS_SLOT_FILE_FMT, dir, id);

    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int zs_pool_entry_reserve(zs_pool *p, uint32_t need)
{
    uint32_t cap;
    zs_entry *e;

    if (p->cap >= need) {
        return 0;
    }
    cap = p->cap ? p->cap : 64u;
    while (cap < need) {
        cap *= 2u;
    }
    e = (zs_entry *)realloc(p->entries, (size_t)cap * sizeof(*e));
    if (!e) {
        return -1;
    }
    p->entries = e;
    p->cap = cap;
    return 0;
}

static zs_entry *zs_pool_find_id(zs_pool *p, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < p->count; ++i) {
        if (p->entries[i].id == id) {
            return &p->entries[i];
        }
    }
    return NULL;
}

static void zs_pool_entry_from_slot(zs_entry *e, zs_slot *s)
{
    e->state = zs_slot_state(s);
    e->seg_start_ms = zs_slot_segment_start_ms(s);
    e->seg_end_ms = zs_slot_segment_end_ms(s);
    snprintf(e->stream, sizeof(e->stream), "%s", zs_slot_stream(s));
}

/* 把一个已存在的槽位文件读进元数据表；上次未封口的先恢复再封口。 */
static void zs_pool_add_scanned(zs_pool *p, uint32_t id)
{
    char path[ZS_POOL_PATH_MAX];
    zs_slot *s;
    zs_entry *e;

    if (zs_pool_path(path, sizeof(path), p->dir, id) != 0) {
        return;
    }
    s = zs_slot_open(path);
    if (!s) {
        return; /* 头损坏的槽位跳过，不删除用户数据 */
    }
    if (zs_pool_entry_reserve(p, p->count + 1u) != 0) {
        zs_slot_close(s);
        return;
    }
    if (zs_slot_state(s) == ZS_SLOT_WRITING) {
        (void)zs_slot_recover(s, 1);
    }
    e = &p->entries[p->count++];
    memset(e, 0, sizeof(*e));
    e->id = id;
    e->channel_id = zs_slot_channel_id(s);
    zs_pool_entry_from_slot(e, s);
    snprintf(e->path, sizeof(e->path), "%s", path);
    zs_slot_close(s);
    if (id >= p->next_id) {
        p->next_id = id + 1u;
    }
}

/* 扫描目录，把已有槽位读进元数据表。 */
static void zs_pool_scan(zs_pool *p)
{
#if defined(_WIN32)
    char pattern[ZS_POOL_PATH_MAX];
    struct _finddata_t fd;
    intptr_t h;

    if (snprintf(pattern, sizeof(pattern), "%s/*.zslot", p->dir) <= 0) {
        return;
    }
    h = _findfirst(pattern, &fd);
    if (h == -1) {
        return;
    }
    do {
        unsigned int id = 0;

        if (fd.name[0] == '.') {
            continue;
        }
        if (sscanf(fd.name, "slot_%8u.zslot", &id) != 1) {
            continue;
        }
        zs_pool_add_scanned(p, id);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(p->dir);
    struct dirent *ent;

    if (!d) {
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        unsigned int id = 0;

        if (ent->d_name[0] == '.') {
            continue;
        }
        if (sscanf(ent->d_name, "slot_%8u.zslot", &id) != 1) {
            continue;
        }
        zs_pool_add_scanned(p, id);
    }
    closedir(d);
#endif
}

/* 逐级创建目录（./www/zstorage 这类两级路径，父目录可能不存在）。 */
static void zs_pool_mkdir_p(const char *dir)
{
    char tmp[ZS_POOL_PATH_MAX];
    char *p;
    size_t n;

    if (!dir || !dir[0]) {
        return;
    }
    n = strlen(dir);
    if (n >= sizeof(tmp)) {
        return;
    }
    memcpy(tmp, dir, n + 1u);
    for (p = tmp + 1; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            char c = *p;

            *p = '\0';
            (void)ZS_POOL_MKDIR(tmp);
            *p = c;
        }
    }
    (void)ZS_POOL_MKDIR(tmp);
}

zs_pool *zs_pool_open(const zs_pool_opts *opts)
{
    zs_pool *p;
    zs_pool_opts def;
    const char *dir;
    uint64_t slot_bytes;
    uint64_t max_slots;

    if (!opts || !opts->dir || !opts->dir[0]) {
        return NULL;
    }
    zs_pool_opts_default(&def);
    dir = opts->dir;
    if (strlen(dir) >= ZS_POOL_PATH_MAX - 32u) {
        return NULL;
    }
    slot_bytes = opts->slot_bytes ? opts->slot_bytes : def.slot_bytes;
    max_slots = (opts->capacity_bytes ? opts->capacity_bytes : def.capacity_bytes) / slot_bytes;
    if (max_slots < 1u) {
        max_slots = 1u;
    }
    if (max_slots > ZS_POOL_MAX_SLOTS) {
        max_slots = ZS_POOL_MAX_SLOTS;
    }

    p = (zs_pool *)calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    snprintf(p->dir, sizeof(p->dir), "%s", dir);
    p->capacity_bytes = opts->capacity_bytes ? opts->capacity_bytes : def.capacity_bytes;
    p->retention_days = opts->retention_days ? opts->retention_days : def.retention_days;
    p->slot_bytes = slot_bytes;
    p->index_bytes = opts->index_bytes ? opts->index_bytes : def.index_bytes;
    p->watermark_pct = opts->free_watermark_pct ? opts->free_watermark_pct : def.free_watermark_pct;
    p->max_slots = (uint32_t)max_slots;
    p->next_id = 1u;
    p->mu = zb_mutex_create(0);
    if (!p->mu) {
        free(p);
        return NULL;
    }

    zs_pool_mkdir_p(p->dir); /* 父目录不存在时补齐 */
    zs_pool_scan(p);
    return p;
}

void zs_pool_close(zs_pool *p)
{
    if (!p) {
        return;
    }
    if (p->mu) {
        zb_mutex_destroy(p->mu);
    }
    free(p->entries);
    free(p);
}

static int zs_pool_reuse(zs_pool *p, zs_entry *e, uint32_t channel_id, const char *stream)
{
    zs_slot *s = zs_slot_open(e->path);

    if (!s) {
        return -1;
    }
    if (zs_slot_reset(s) != 0 || zs_slot_begin(s, channel_id, stream) != 0) {
        zs_slot_close(s);
        return -1;
    }
    e->channel_id = channel_id;
    e->state = ZS_SLOT_WRITING;
    e->ref_count = 0;
    e->seg_start_ms = 0;
    e->seg_end_ms = 0;
    snprintf(e->stream, sizeof(e->stream), "%s", stream ? stream : "");
    zs_slot_close(s);
    return 0;
}

static int zs_pool_create_new(zs_pool *p, uint32_t channel_id, const char *stream, zs_entry *e)
{
    zs_slot_opts o;
    zs_slot *s;
    uint32_t id = p->next_id++;

    if (zs_pool_path(e->path, sizeof(e->path), p->dir, id) != 0) {
        return -1;
    }
    memset(&o, 0, sizeof(o));
    o.channel_id = channel_id;
    o.slot_seq = id;
    o.stream = stream;
    o.slot_bytes = p->slot_bytes;
    o.index_bytes = p->index_bytes;
    s = zs_slot_create(e->path, &o);
    if (!s) {
        return -1;
    }
    zs_slot_close(s);
    e->id = id;
    e->channel_id = channel_id;
    e->state = ZS_SLOT_WRITING;
    e->ref_count = 0;
    e->seg_start_ms = 0;
    e->seg_end_ms = 0;
    snprintf(e->stream, sizeof(e->stream), "%s", stream ? stream : "");
    return 0;
}

/* 找最旧的、可回收的 SEALED 槽位。 */
static zs_entry *zs_pool_oldest_evictable(zs_pool *p)
{
    zs_entry *best = NULL;
    uint32_t i;

    for (i = 0; i < p->count; ++i) {
        zs_entry *e = &p->entries[i];

        if (e->state != ZS_SLOT_SEALED || e->ref_count != 0) {
            continue;
        }
        if (!best || e->seg_start_ms < best->seg_start_ms) {
            best = e;
        }
    }
    return best;
}

static int zs_pool_acquire_nolock(zs_pool *p, uint32_t channel_id, const char *stream,
                                  uint64_t now_ms, zs_pool_lease *out)
{
    zs_entry *e = NULL;
    uint32_t i;
    zs_slot *s;

    if (!p || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    (void)now_ms;

    for (i = 0; i < p->count; ++i) {
        if (p->entries[i].state == ZS_SLOT_FREE) {
            e = &p->entries[i];
            break;
        }
    }
    if (e) {
        if (zs_pool_reuse(p, e, channel_id, stream) != 0) {
            return -1;
        }
    } else if (p->count < p->max_slots) {
        if (zs_pool_entry_reserve(p, p->count + 1u) != 0) {
            return -1;
        }
        e = &p->entries[p->count];
        memset(e, 0, sizeof(*e));
        if (zs_pool_create_new(p, channel_id, stream, e) != 0) {
            return -1;
        }
        p->count++;
    } else {
        e = zs_pool_oldest_evictable(p);
        if (!e || zs_pool_reuse(p, e, channel_id, stream) != 0) {
            return -1;
        }
    }

    s = zs_slot_open(e->path);
    if (!s) {
        return -1;
    }
    /* 新建路径已是 WRITING；复用路径刚 begin 过，这里统一把通道绑一次 */
    if (zs_slot_channel_id(s) != channel_id || strcmp(zs_slot_stream(s), stream ? stream : "") != 0) {
        if (zs_slot_begin(s, channel_id, stream) != 0) {
            zs_slot_close(s);
            return -1;
        }
    }
    out->id = e->id;
    out->slot = s;
    return 0;
}

int zs_pool_acquire(zs_pool *p, uint32_t channel_id, const char *stream, uint64_t now_ms,
                    zs_pool_lease *out)
{
    int rc;

    ZS_POOL_LOCK(p);
    rc = zs_pool_acquire_nolock(p, channel_id, stream, now_ms, out);
    ZS_POOL_UNLOCK(p);
    return rc;
}

static int zs_pool_release_nolock(zs_pool *p, zs_pool_lease *lease)
{
    zs_entry *e;

    if (!p || !lease || !lease->slot) {
        return -1;
    }
    e = zs_pool_find_id(p, lease->id);
    if (zs_slot_keyframe_count(lease->slot) == 0) {
        (void)zs_slot_reset(lease->slot);
    } else {
        (void)zs_slot_seal(lease->slot);
    }
    if (e) {
        zs_pool_entry_from_slot(e, lease->slot);
        if (e->state == ZS_SLOT_FREE) {
            e->channel_id = 0;
            e->seg_start_ms = 0;
            e->seg_end_ms = 0;
        }
    }
    zs_slot_close(lease->slot);
    lease->slot = NULL;
    return 0;
}

int zs_pool_release(zs_pool *p, zs_pool_lease *lease)
{
    int rc;

    ZS_POOL_LOCK(p);
    rc = zs_pool_release_nolock(p, lease);
    ZS_POOL_UNLOCK(p);
    return rc;
}

#define ZS_LOCK_C(p) ZS_POOL_LOCK((zs_pool *)(uintptr_t)(p))
#define ZS_UNLOCK_C(p) ZS_POOL_UNLOCK((zs_pool *)(uintptr_t)(p))

int zs_pool_sync(zs_pool *p, zs_pool_lease *lease)
{
    zs_entry *e;
    int rc = 0;

    if (!p || !lease || !lease->slot) {
        return -1;
    }
    (void)zs_slot_flush(lease->slot);
    ZS_POOL_LOCK(p);
    e = zs_pool_find_id(p, lease->id);
    if (e) {
        zs_pool_entry_from_slot(e, lease->slot);
    } else {
        rc = -1;
    }
    ZS_POOL_UNLOCK(p);
    return rc;
}

int zs_pool_pin(zs_pool *p, uint32_t id)
{
    zs_entry *e;
    int rc = -1;

    ZS_POOL_LOCK(p);
    e = p ? zs_pool_find_id(p, id) : NULL;
    if (e) {
        e->ref_count++;
        rc = 0;
    }
    ZS_POOL_UNLOCK(p);
    return rc;
}

int zs_pool_unpin(zs_pool *p, uint32_t id)
{
    zs_entry *e;
    int rc = -1;

    ZS_POOL_LOCK(p);
    e = p ? zs_pool_find_id(p, id) : NULL;
    if (e && e->ref_count > 0) {
        e->ref_count--;
        rc = 0;
    }
    ZS_POOL_UNLOCK(p);
    return rc;
}

uint32_t zs_pool_expire(zs_pool *p, uint64_t now_ms)
{
    uint64_t retention_ms;
    uint32_t i;
    uint32_t n = 0;

    if (!p || p->retention_days == 0) {
        return 0;
    }
    retention_ms = (uint64_t)p->retention_days * ZS_DAY_MS;
    ZS_POOL_LOCK(p);
    for (i = 0; i < p->count; ++i) {
        zs_entry *e = &p->entries[i];
        zs_slot *s;

        if (e->state != ZS_SLOT_SEALED || e->ref_count != 0 || e->seg_end_ms == 0) {
            continue;
        }
        if (e->seg_end_ms + retention_ms > now_ms) {
            continue;
        }
        s = zs_slot_open(e->path);
        if (s) {
            (void)zs_slot_reset(s);
            zs_slot_close(s);
        }
        e->state = ZS_SLOT_FREE;
        e->channel_id = 0;
        e->stream[0] = '\0';
        e->seg_start_ms = 0;
        e->seg_end_ms = 0;
        n++;
    }
    ZS_POOL_UNLOCK(p);
    return n;
}

uint32_t zs_pool_count(const zs_pool *p)
{
    uint32_t n;

    ZS_LOCK_C(p);
    n = p ? p->count : 0;
    ZS_UNLOCK_C(p);
    return n;
}

uint32_t zs_pool_free_count(const zs_pool *p)
{
    uint32_t i;
    uint32_t n = 0;

    ZS_LOCK_C(p);
    for (i = 0; p && i < p->count; ++i) {
        if (p->entries[i].state == ZS_SLOT_FREE) {
            n++;
        }
    }
    ZS_UNLOCK_C(p);
    return n;
}

uint32_t zs_pool_max_slots(const zs_pool *p)
{
    return p ? p->max_slots : 0;
}

uint32_t zs_pool_channel_count(const zs_pool *p, uint32_t channel_id)
{
    uint32_t i;
    uint32_t n = 0;

    if (!p || channel_id == 0) {
        return 0;
    }
    ZS_LOCK_C(p);
    for (i = 0; i < p->count; ++i) {
        if (p->entries[i].channel_id == channel_id && p->entries[i].state != ZS_SLOT_FREE) {
            n++;
        }
    }
    ZS_UNLOCK_C(p);
    return n;
}

uint64_t zs_pool_used_bytes(const zs_pool *p)
{
    uint32_t i;
    uint64_t n = 0;

    if (!p) {
        return 0;
    }
    ZS_LOCK_C(p);
    for (i = 0; i < p->count; ++i) {
        if (p->entries[i].state != ZS_SLOT_FREE) {
            n += p->slot_bytes;
        }
    }
    ZS_UNLOCK_C(p);
    return n;
}

uint64_t zs_pool_capacity_bytes(const zs_pool *p)
{
    return p ? (uint64_t)p->max_slots * p->slot_bytes : 0;
}

int zs_pool_get_info(const zs_pool *p, uint32_t idx, zs_pool_info *out)
{
    int rc = -1;

    ZS_LOCK_C(p);
    if (p && out && idx < p->count) {
        const zs_entry *e = &p->entries[idx];

        out->id = e->id;
        out->channel_id = e->channel_id;
        out->state = e->state;
        out->ref_count = e->ref_count;
        out->seg_start_ms = e->seg_start_ms;
        out->seg_end_ms = e->seg_end_ms;
        snprintf(out->stream, sizeof(out->stream), "%s", e->stream);
        rc = 0;
    }
    ZS_UNLOCK_C(p);
    return rc;
}

int zs_pool_get_info_by_id(const zs_pool *p, uint32_t id, zs_pool_info *out)
{
    uint32_t i;
    int rc = -1;

    ZS_LOCK_C(p);
    for (i = 0; p && i < p->count; ++i) {
        if (p->entries[i].id == id) {
            const zs_entry *e = &p->entries[i];

            if (out) {
                out->id = e->id;
                out->channel_id = e->channel_id;
                out->state = e->state;
                out->ref_count = e->ref_count;
                out->seg_start_ms = e->seg_start_ms;
                out->seg_end_ms = e->seg_end_ms;
                snprintf(out->stream, sizeof(out->stream), "%s", e->stream);
                rc = 0;
            }
            break;
        }
    }
    ZS_UNLOCK_C(p);
    return rc;
}

uint32_t zs_pool_find_range(const zs_pool *p, uint64_t from_ms, uint64_t to_ms, uint32_t *ids,
                            uint32_t max)
{
    return zs_pool_find_range_stream(p, NULL, from_ms, to_ms, ids, max);
}

uint32_t zs_pool_find_range_stream(const zs_pool *p, const char *stream, uint64_t from_ms,
                                   uint64_t to_ms, uint32_t *ids, uint32_t max)
{
    const zs_entry **tmp;
    uint32_t i;
    uint32_t n = 0;

    if (!p || !ids || max == 0) {
        return 0;
    }
    tmp = (const zs_entry **)malloc((size_t)max * sizeof(*tmp));
    if (!tmp) {
        return 0;
    }
    ZS_LOCK_C(p);
    for (i = 0; i < p->count && n < max; ++i) {
        const zs_entry *e = &p->entries[i];

        if (e->state == ZS_SLOT_FREE || e->seg_end_ms == 0) {
            continue;
        }
        if (stream && stream[0] && strcmp(e->stream, stream) != 0) {
            continue;
        }
        if (e->seg_start_ms > to_ms || e->seg_end_ms < from_ms) {
            continue;
        }
        tmp[n++] = e;
    }
    ZS_UNLOCK_C(p);

    /* 按 seg_start 升序插入排序（n 通常很小） */
    for (i = 1; i < n; ++i) {
        const zs_entry *key = tmp[i];
        uint32_t j = i;

        while (j > 0 && tmp[j - 1]->seg_start_ms > key->seg_start_ms) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = key;
    }
    for (i = 0; i < n; ++i) {
        ids[i] = tmp[i]->id;
    }
    free((void *)tmp);
    return n;
}

zs_slot *zs_pool_open_slot(zs_pool *p, uint32_t id)
{
    char path[ZS_POOL_PATH_MAX];
    zs_entry *e;
    int ok = 0;

    if (!p) {
        return NULL;
    }
    ZS_POOL_LOCK(p);
    e = zs_pool_find_id(p, id);
    if (e && zs_pool_path(path, sizeof(path), p->dir, id) == 0) {
        ok = 1;
    }
    ZS_POOL_UNLOCK(p);
    return ok ? zs_slot_open(path) : NULL;
}
