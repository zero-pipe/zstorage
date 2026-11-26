/**
 * @file device_ptz.c
 * @brief PTZ 命令分发与事件记录。
 */
#include "zms/device/core/device_ptz.h"

#include "zms/device/core/device_event.h"
#include "zb/os/sync.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <string.h>

static struct {
    zms_device_ptz_handler fn;
    void *user;
    zb_mutex *mtx;
    int inited;
} g_ptz;

static void ensure_init(void)
{
    if (g_ptz.inited) {
        return;
    }
    g_ptz.mtx = zb_mutex_create(ZB_MUTEX_NORMAL);
    g_ptz.inited = g_ptz.mtx ? 1 : 0;
}

void zms_device_ptz_init(void)
{
    ensure_init();
}

void zms_device_ptz_fini(void)
{
    if (!g_ptz.inited) {
        return;
    }
    zb_mutex_lock(g_ptz.mtx);
    g_ptz.fn = NULL;
    g_ptz.user = NULL;
    zb_mutex_unlock(g_ptz.mtx);
    zb_mutex_destroy(g_ptz.mtx);
    g_ptz.mtx = NULL;
    g_ptz.inited = 0;
}

int zms_device_ptz_set_handler(zms_device_ptz_handler fn, void *user)
{
    ensure_init();
    if (!g_ptz.mtx) {
        return -1;
    }
    zb_mutex_lock(g_ptz.mtx);
    g_ptz.fn = fn;
    g_ptz.user = user;
    zb_mutex_unlock(g_ptz.mtx);
    return 0;
}

int zms_device_ptz_has_backend(void)
{
    int has;
    ensure_init();
    if (!g_ptz.mtx) {
        return 0;
    }
    zb_mutex_lock(g_ptz.mtx);
    has = g_ptz.fn != NULL;
    zb_mutex_unlock(g_ptz.mtx);
    return has;
}

int zms_device_ptz_dispatch(const zms_device_ptz_cmd *cmd)
{
    zms_device_ptz_handler fn;
    void *user;
    int rc = 0;
    zms_device_event ev;

    if (!cmd) {
        return -1;
    }
    ensure_init();
    if (!g_ptz.mtx) {
        return -1;
    }
    zb_mutex_lock(g_ptz.mtx);
    fn = g_ptz.fn;
    user = g_ptz.user;
    zb_mutex_unlock(g_ptz.mtx);

    if (fn) {
        rc = fn(cmd, user);
    }
    memset(&ev, 0, sizeof(ev));
    ev.type = ZMS_DEV_EVENT_PTZ;
    snprintf(ev.channel_id, sizeof(ev.channel_id), "%s", cmd->channel_id);
    snprintf(ev.message, sizeof(ev.message), "stop=%d pan=%.2f tilt=%.2f zoom=%.2f", cmd->stop,
             cmd->pan, cmd->tilt, cmd->zoom);
    zms_device_event_publish(&ev);
    if (!fn) {
        zn_info("device_ptz: no backend, accepted channel=%s pan=%.2f tilt=%.2f zoom=%.2f stop=%d",
                 cmd->channel_id, cmd->pan, cmd->tilt, cmd->zoom, cmd->stop);
    }
    return rc;
}
