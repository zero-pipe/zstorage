/**
 * @file device_event.c
 * @brief 设备事件环形缓冲与订阅。
 */
#include "zms/device/core/device_event.h"

#include "zb/os/sync.h"
#include "zn/util/timer.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    zms_device_event_cb cb;
    void *user;
} subscriber;

static struct {
    zms_device_event ring[ZMS_DEVICE_EVENT_RING];
    size_t write_idx;
    size_t count;
    subscriber subs[ZMS_DEVICE_EVENT_SUBSCRIBERS];
    zb_mutex *mtx;
    int inited;
} g_ev;

static void ensure_init(void)
{
    if (g_ev.inited) {
        return;
    }
    g_ev.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    g_ev.inited = g_ev.mtx ? 1 : 0;
}

void zms_device_event_init(void)
{
    ensure_init();
}

void zms_device_event_fini(void)
{
    if (!g_ev.inited) {
        return;
    }
    zb_mutex_lock(g_ev.mtx);
    memset(&g_ev.ring, 0, sizeof(g_ev.ring));
    memset(&g_ev.subs, 0, sizeof(g_ev.subs));
    g_ev.write_idx = 0;
    g_ev.count = 0;
    zb_mutex_unlock(g_ev.mtx);
    zb_mutex_destroy(g_ev.mtx);
    g_ev.mtx = NULL;
    g_ev.inited = 0;
}

void zms_device_event_publish(const zms_device_event *ev)
{
    zms_device_event e;
    subscriber subs[ZMS_DEVICE_EVENT_SUBSCRIBERS];
    size_t n = 0;
    size_t i;

    if (!ev) {
        return;
    }
    ensure_init();
    if (!g_ev.mtx) {
        return;
    }
    e = *ev;
    if (e.time_ms == 0) {
        e.time_ms = zn_wall_ms();
    }
    zb_mutex_lock(g_ev.mtx);
    g_ev.ring[g_ev.write_idx % ZMS_DEVICE_EVENT_RING] = e;
    ++g_ev.write_idx;
    if (g_ev.count < ZMS_DEVICE_EVENT_RING) {
        ++g_ev.count;
    }
    for (i = 0; i < ZMS_DEVICE_EVENT_SUBSCRIBERS; ++i) {
        if (g_ev.subs[i].cb) {
            subs[n++] = g_ev.subs[i];
        }
    }
    zb_mutex_unlock(g_ev.mtx);
    for (i = 0; i < n; ++i) {
        subs[i].cb(&e, subs[i].user);
    }
}

size_t zms_device_event_list(zms_device_event *out, size_t cap)
{
    size_t n;
    size_t i;

    if (!out || cap == 0) {
        return 0;
    }
    ensure_init();
    if (!g_ev.mtx) {
        return 0;
    }
    zb_mutex_lock(g_ev.mtx);
    n = g_ev.count < cap ? g_ev.count : cap;
    for (i = 0; i < n; ++i) {
        size_t idx = (g_ev.write_idx + ZMS_DEVICE_EVENT_RING - 1 - i) % ZMS_DEVICE_EVENT_RING;
        out[i] = g_ev.ring[idx];
    }
    zb_mutex_unlock(g_ev.mtx);
    return n;
}

int zms_device_event_subscribe(zms_device_event_cb cb, void *user)
{
    size_t i;

    if (!cb) {
        return -1;
    }
    ensure_init();
    if (!g_ev.mtx) {
        return -1;
    }
    zb_mutex_lock(g_ev.mtx);
    for (i = 0; i < ZMS_DEVICE_EVENT_SUBSCRIBERS; ++i) {
        if (!g_ev.subs[i].cb) {
            g_ev.subs[i].cb = cb;
            g_ev.subs[i].user = user;
            zb_mutex_unlock(g_ev.mtx);
            return 0;
        }
    }
    zb_mutex_unlock(g_ev.mtx);
    return -1;
}

int zms_device_event_unsubscribe(zms_device_event_cb cb, void *user)
{
    size_t i;

    ensure_init();
    if (!g_ev.mtx) {
        return -1;
    }
    zb_mutex_lock(g_ev.mtx);
    for (i = 0; i < ZMS_DEVICE_EVENT_SUBSCRIBERS; ++i) {
        if (g_ev.subs[i].cb == cb && g_ev.subs[i].user == user) {
            g_ev.subs[i].cb = NULL;
            g_ev.subs[i].user = NULL;
            zb_mutex_unlock(g_ev.mtx);
            return 0;
        }
    }
    zb_mutex_unlock(g_ev.mtx);
    return -1;
}

const char *zms_device_event_type_name(int type)
{
    switch (type) {
    case ZMS_DEV_EVENT_ALARM:
        return "alarm";
    case ZMS_DEV_EVENT_MOTION:
        return "motion";
    case ZMS_DEV_EVENT_ONLINE:
        return "online";
    case ZMS_DEV_EVENT_OFFLINE:
        return "offline";
    case ZMS_DEV_EVENT_STREAM_START:
        return "stream_start";
    case ZMS_DEV_EVENT_STREAM_STOP:
        return "stream_stop";
    case ZMS_DEV_EVENT_PTZ:
        return "ptz";
    default:
        return "unknown";
    }
}
