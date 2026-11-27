/**
 * @file egress_zs_record_task.c
 * @brief 云存储录制任务：按通道主动拉流 -> 就绪即录 -> 断流封口。
 *
 * 设计要点（与"见流就录"的旁路钩子相反）：
 *   - 平台下发"录某路/某通道"，给 channel_key（+可选 rtsp_url）；
 *   - 每个任务持有一个拉流代理，把该路从 ZMS 拉到本进程的 media_source；
 *   - 代理 on_ready（连上且已 publish）时，attach 云存储录制器到该源；
 *   - 代理 on_error 只做重连，不动录制器：断流由 publish_fini 触发的
 *     zs_recorder_stop 封口，重连后源复用、录到新槽位；
 *   - 删除任务 proxy_destroy -> publish_fini -> 封口 -> 释放，次序一致。
 *
 * 任务表以 channel_key 为主键，互斥保护；查询/遍历返回副本，不暴露内部指针。
 */
#include "zms/egress/recorder/zs_record_task.h"

#include "zms/egress/recorder/egress_zs_recorder.h"
#include "zms/pull/proxy.h"
#include "zms/core/event.h"
#include "zms/core/stream/hub.h"
#include "zms/media/clock.h"
#include "storage/zs_pool.h"
#include "zb/os/sync.h"
#include "zn/platform.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZS_TASK_MAX 1024

typedef struct zs_task {
    char channel_key[ZS_CHANNEL_KEY_MAX];
    char pull_url[768];
    zms_live_pull_proxy *proxy;
    uint64_t started_utc_ms;
    int online; /* 已 publish（on_ready 置位，stop/destroy 清除） */
    int used;
} zs_task;

static struct {
    zs_task tasks[ZS_TASK_MAX];
    uint32_t count;
    zb_mutex *mu;
} g_t;

static zb_mutex *task_mutex(void)
{
    if (!g_t.mu) {
        g_t.mu = zb_mutex_create(0);
    }
    return g_t.mu;
}

/* ── 代理回调 ───────────────────────────────────────────────────────────── */

/* on_ready：连上并 publish 后，把云存储录制器挂到该源的 GOP 上。 */
static void task_on_ready(void *user)
{
    zs_task *t = (zs_task *)user;
    zms_media_source *src;
    zn_err_t rc;

    if (!t || !t->proxy) {
        return;
    }
    src = zms_live_pull_proxy_source(t->proxy);
    if (!src || !src->gop_queue) {
        zn_warn("[zs] task ready 但源未就绪 key=%s", t->channel_key);
        return;
    }
    t->online = 1;
    rc = zms_zs_recorder_start(src, t->channel_key);
    if (rc != ZN_OK) {
        zn_warn("[zs] 录制器启动失败 key=%s rc=%d", t->channel_key, (int)rc);
    } else {
        zn_info("[zs] 录制任务已就绪 key=%s", t->channel_key);
    }
}

/* on_error：只记日志。断流封口与重连由代理内部 + publish_fini 处理。 */
static void task_on_error(zn_err_t err, void *user)
{
    zs_task *t = (zs_task *)user;

    if (t) {
        zn_warn("[zs] 拉流异常 key=%s err=%d（将由代理自动重连）", t->channel_key, (int)err);
    }
}

/* ── URL 拼装 ───────────────────────────────────────────────────────────── */

static void task_build_default_url(const char *channel_key, char *out, size_t cap)
{
    const zms_storage_config *cfg = NULL;
    const char *host = "127.0.0.1";
    int port = 554;

    /* 从录制器拿当前存储配置里的 pull_host/pull_port。配置不可直取时用默认。 */
    cfg = zms_zs_recorder_config();
    if (cfg) {
        if (cfg->pull_host[0]) {
            host = cfg->pull_host;
        }
        if (cfg->pull_port > 0) {
            port = cfg->pull_port;
        }
    }
    snprintf(out, cap, "rtsp://%s:%d/live/%s", host, port, channel_key);
}

/* ── 任务表查找 ─────────────────────────────────────────────────────────── */

static zs_task *task_find(const char *channel_key)
{
    uint32_t i;

    if (!channel_key || !channel_key[0]) {
        return NULL;
    }
    for (i = 0; i < g_t.count; ++i) {
        if (g_t.tasks[i].used && strcmp(g_t.tasks[i].channel_key, channel_key) == 0) {
            return &g_t.tasks[i];
        }
    }
    return NULL;
}

static zs_task *task_alloc(void)
{
    uint32_t i;

    for (i = 0; i < g_t.count; ++i) {
        if (!g_t.tasks[i].used) {
            return &g_t.tasks[i];
        }
    }
    if (g_t.count >= ZS_TASK_MAX) {
        return NULL;
    }
    return &g_t.tasks[g_t.count++];
}

/* ── 生命周期 ───────────────────────────────────────────────────────────── */

zn_err_t zms_zs_record_start(const char *channel_key, const char *pull_url)
{
    zb_mutex *mu;
    zs_task *t;
    char url[768];
    zn_poller *pol;
    zms_live_pull_proxy_opts popts;
    zms_live_pull_proxy *proxy;
    int is_new = 0;

    if (!channel_key || !channel_key[0] || strlen(channel_key) >= ZS_CHANNEL_KEY_MAX) {
        return ZN_ERR_INVALID;
    }
    if (!zms_zs_recorder_enabled()) {
        return ZN_ERR_STATE;
    }
    if (pull_url && pull_url[0]) {
        snprintf(url, sizeof(url), "%s", pull_url);
    } else {
        task_build_default_url(channel_key, url, sizeof(url));
    }
    pol = zms_media_events_poller();
    if (!pol) {
        return ZN_ERR_STATE;
    }

    mu = task_mutex();
    if (!mu) {
        return ZN_ERR_NOMEM;
    }
    zb_mutex_lock(mu);

    t = task_find(channel_key);
    if (t) {
        /* 已在运行：URL 未变则幂等返回；URL 变了重建代理（源头换地址）。 */
        if (strcmp(t->pull_url, url) == 0) {
            zb_mutex_unlock(mu);
            return ZN_OK;
        }
        if (t->proxy) {
            zms_live_pull_proxy_destroy(t->proxy); /* 触发封口/释放录制器 */
            t->proxy = NULL;
        }
        t->online = 0;
        snprintf(t->pull_url, sizeof(t->pull_url), "%s", url);
        is_new = 0;
    } else {
        t = task_alloc();
        if (!t) {
            zb_mutex_unlock(mu);
            zn_error("[zs] 录制任务已达上限 %d", ZS_TASK_MAX);
            return ZN_ERR_NOMEM;
        }
        memset(t, 0, sizeof(*t));
        t->used = 1;
        snprintf(t->channel_key, sizeof(t->channel_key), "%s", channel_key);
        snprintf(t->pull_url, sizeof(t->pull_url), "%s", url);
        t->started_utc_ms = zms_wall_ms();
        is_new = 1;
    }

    memset(&popts, 0, sizeof(popts));
    popts.poller = pol;
    popts.pull_url = t->pull_url;
    popts.app = "live";
    popts.stream = channel_key;
    popts.rtp_mode = ZMS_RTSP_RTP_TCP; /* 与 ZMS 内网拉流默认一致 */
    popts.retry_count = -1;            /* 无限重连：录制任务存活即一直录 */
    popts.reconnect_delay_ms = 3000;
    popts.on_ready = task_on_ready;
    popts.on_error = task_on_error;
    popts.user = t;

    proxy = zms_live_pull_proxy_create(&popts);
    if (!proxy) {
        if (is_new) {
            memset(t, 0, sizeof(*t)); /* 全新任务创建失败则回收槽位 */
            while (g_t.count > 0 && !g_t.tasks[g_t.count - 1].used) {
                g_t.count--;
            }
        }
        zb_mutex_unlock(mu);
        return ZN_ERR_STATE;
    }
    t->proxy = proxy;
    zb_mutex_unlock(mu);

    /* 在锁外启动拉流：play 过程可能同步回调 on_ready，不能持锁。 */
    if (zms_live_pull_proxy_start(proxy) != ZN_OK) {
        return ZN_ERR_STATE; /* 代理会按 retry 自行重连，不算硬失败 */
    }
    zn_info("[zs] 录制任务已创建 key=%s url=%s", channel_key, url);
    return ZN_OK;
}

void zms_zs_record_stop(const char *channel_key)
{
    zb_mutex *mu;
    zs_task *t;
    zms_live_pull_proxy *proxy;

    if (!channel_key || !channel_key[0]) {
        return;
    }
    mu = task_mutex();
    if (!mu) {
        return;
    }
    zb_mutex_lock(mu);
    t = task_find(channel_key);
    if (!t) {
        zb_mutex_unlock(mu);
        return;
    }
    proxy = t->proxy;
    t->proxy = NULL;
    t->online = 0;
    memset(t, 0, sizeof(*t));
    /* 收缩尾部空洞 */
    while (g_t.count > 0 && !g_t.tasks[g_t.count - 1].used) {
        g_t.count--;
    }
    zb_mutex_unlock(mu);

    /* 锁外销毁：proxy_destroy -> publish_fini -> 录制器封口 -> 释放。 */
    if (proxy) {
        zms_live_pull_proxy_destroy(proxy);
    }
    zn_info("[zs] 录制任务已停止 key=%s", channel_key);
}

/* ── 查询 ───────────────────────────────────────────────────────────────── */

static void task_fill_info(const zs_task *t, zs_record_task_info *out)
{
    snprintf(out->channel_key, sizeof(out->channel_key), "%s", t->channel_key);
    snprintf(out->pull_url, sizeof(out->pull_url), "%s", t->pull_url);
    out->recording = t->proxy && t->online ? 1 : 0;
    out->online = t->online;
    out->started_utc_ms = t->started_utc_ms;
}

int zms_zs_record_get(const char *channel_key, zs_record_task_info *out)
{
    zb_mutex *mu = task_mutex();
    zs_task *t;

    if (!mu || !out) {
        return 0;
    }
    zb_mutex_lock(mu);
    t = task_find(channel_key);
    if (!t) {
        zb_mutex_unlock(mu);
        return 0;
    }
    task_fill_info(t, out);
    zb_mutex_unlock(mu);
    return 1;
}

uint32_t zs_record_task_count(void)
{
    zb_mutex *mu = task_mutex();
    uint32_t i;
    uint32_t n = 0;

    if (!mu) {
        return 0;
    }
    zb_mutex_lock(mu);
    for (i = 0; i < g_t.count; ++i) {
        if (g_t.tasks[i].used) {
            n++;
        }
    }
    zb_mutex_unlock(mu);
    return n;
}

int zs_record_task_at(uint32_t idx, zs_record_task_info *out)
{
    zb_mutex *mu = task_mutex();
    uint32_t i;
    uint32_t seen = 0;
    int found = 0;

    if (!mu || !out) {
        return 0;
    }
    zb_mutex_lock(mu);
    for (i = 0; i < g_t.count; ++i) {
        if (!g_t.tasks[i].used) {
            continue;
        }
        if (seen == idx) {
            task_fill_info(&g_t.tasks[i], out);
            found = 1;
            break;
        }
        seen++;
    }
    zb_mutex_unlock(mu);
    return found;
}

uint32_t zms_zs_record_query(const char *channel_key, uint64_t from_ms, uint64_t to_ms,
                             zs_record_segment *out, uint32_t max)
{
    zs_pool *pool = zms_zs_recorder_pool();
    uint32_t ids[512];
    uint32_t n;
    uint32_t i;
    uint32_t written = 0;
    const char *stream = (channel_key && channel_key[0]) ? channel_key : NULL;

    if (!pool || !out || max == 0) {
        return 0;
    }
    n = zs_pool_find_range_stream(pool, stream, from_ms, to_ms, ids,
                                  (uint32_t)(sizeof(ids) / sizeof(ids[0])));
    for (i = 0; i < n && written < max; ++i) {
        zs_pool_info info;

        if (zs_pool_get_info_by_id(pool, ids[i], &info) != 0) {
            continue;
        }
        out[written].slot_id = info.id;
        snprintf(out[written].channel_key, sizeof(out[written].channel_key), "%s", info.stream);
        out[written].start_ms = info.seg_start_ms;
        out[written].end_ms = info.seg_end_ms;
        out[written].bytes = 0;
        {
            zs_slot *s = zs_pool_open_slot(pool, info.id);
            if (s) {
                uint64_t off = zs_slot_write_offset(s);
                uint64_t ds = zs_slot_data_start(s);
                out[written].bytes = off > ds ? off - ds : 0;
                zs_slot_close(s);
            }
        }
        written++;
    }
    return written;
}

uint32_t zms_zs_record_delete(const uint32_t *slots, uint32_t n_slots)
{
    zs_pool *pool = zms_zs_recorder_pool();
    uint32_t i;
    uint32_t n = 0;

    if (!pool || !slots) {
        return 0;
    }
    for (i = 0; i < n_slots; ++i) {
        if (zs_pool_delete_by_id(pool, slots[i]) == 0) {
            n++;
        }
    }
    return n;
}

void zms_zs_record_node_status(uint32_t *slots, uint32_t *free_slots, uint64_t *used_bytes,
                               uint64_t *cap_bytes)
{
    zs_pool *pool = zms_zs_recorder_pool();

    if (slots) {
        *slots = 0;
    }
    if (free_slots) {
        *free_slots = 0;
    }
    if (used_bytes) {
        *used_bytes = 0;
    }
    if (cap_bytes) {
        *cap_bytes = 0;
    }
    if (!pool) {
        return;
    }
    if (slots) {
        *slots = zs_pool_count(pool);
    }
    if (free_slots) {
        *free_slots = zs_pool_free_count(pool);
    }
    if (used_bytes) {
        *used_bytes = zs_pool_used_bytes(pool);
    }
    if (cap_bytes) {
        *cap_bytes = zs_pool_capacity_bytes(pool);
    }
}

int zms_zs_record_slot_path(uint32_t slot, char *out, size_t cap)
{
    zs_pool *pool = zms_zs_recorder_pool();

    if (!pool) {
        return -1;
    }
    return zs_pool_slot_path(pool, slot, out, cap);
}
