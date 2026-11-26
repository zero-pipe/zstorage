#include "zm/core/track.h"

#include <stdlib.h>
#include <string.h>

void zm_track_init(zm_track *t)
{
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

void zm_track_clear(zm_track *t)
{
    if (!t) {
        return;
    }
    free(t->config.data);
    zm_track_init(t);
}

int zm_track_set_config(zm_track *t, zm_config_fmt fmt, const void *data, size_t len)
{
    uint8_t *copy = NULL;

    if (!t || (len && !data)) {
        return ZM_ERR_INVALID;
    }
    if (fmt == ZM_CONFIG_NONE || len == 0) {
        free(t->config.data);
        t->config.data = NULL;
        t->config.len = 0;
        t->config.fmt = ZM_CONFIG_NONE;
        return ZM_OK;
    }
    copy = (uint8_t *)malloc(len);
    if (!copy) {
        return ZM_ERR_NOMEM;
    }
    memcpy(copy, data, len);
    free(t->config.data);
    t->config.data = copy;
    t->config.len = len;
    t->config.fmt = fmt;
    return ZM_OK;
}

int zm_config_copy(zm_config *dst, const zm_config *src)
{
    uint8_t *copy = NULL;

    if (!dst || !src) {
        return ZM_ERR_INVALID;
    }
    if (src->len == 0 || !src->data) {
        free(dst->data);
        dst->data = NULL;
        dst->len = 0;
        dst->fmt = ZM_CONFIG_NONE;
        return ZM_OK;
    }
    copy = (uint8_t *)malloc(src->len);
    if (!copy) {
        return ZM_ERR_NOMEM;
    }
    memcpy(copy, src->data, src->len);
    free(dst->data);
    dst->data = copy;
    dst->len = src->len;
    dst->fmt = src->fmt;
    return ZM_OK;
}
