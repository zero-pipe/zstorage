#include "zm/core/mem.h"

#include "zm_atomic.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

typedef struct zm_mem_box {
    volatile int refcnt;
    size_t cap;
    uint8_t data[1];
} zm_mem_box;

#define ZM_MEM_BOX_HDR offsetof(zm_mem_box, data)

static void zm_box_retain(void *owner)
{
    zm_mem_box *box = (zm_mem_box *)owner;
    if (box) {
        zm_atomic_inc(&box->refcnt);
    }
}

static void zm_box_release(void *owner)
{
    zm_mem_box *box = (zm_mem_box *)owner;
    if (box && zm_atomic_dec(&box->refcnt) == 0) {
        free(box);
    }
}

zm_mem zm_mem_borrow(const void *data, size_t len)
{
    zm_mem m;
    memset(&m, 0, sizeof(m));
    m.data = (uint8_t *)(uintptr_t)data;
    m.len = data ? len : 0;
    m.cap = m.len;
    return m;
}

zm_mem zm_mem_wrap(void *owner, void (*retain)(void *owner),
                     void (*release)(void *owner), void *data, size_t len, size_t cap)
{
    zm_mem m;
    memset(&m, 0, sizeof(m));
    m.owner = owner;
    m.retain = retain;
    m.release = release;
    m.data = (uint8_t *)data;
    m.len = data ? len : 0;
    m.cap = cap > m.len ? cap : m.len;
    return m;
}

void zm_mem_ref(zm_mem *m)
{
    if (m && m->owner && m->retain) {
        m->retain(m->owner);
    }
}

void zm_mem_unref(zm_mem *m)
{
    if (!m) {
        return;
    }
    if (m->owner && m->release) {
        m->release(m->owner);
    }
    memset(m, 0, sizeof(*m));
}

int zm_mem_alloc(size_t cap, zm_mem *out)
{
    zm_mem_box *box;
    size_t bytes;

    if (!out) {
        return ZM_ERR_INVALID;
    }
    memset(out, 0, sizeof(*out));
    bytes = ZM_MEM_BOX_HDR + (cap ? cap : 1);
    box = (zm_mem_box *)malloc(bytes);
    if (!box) {
        return ZM_ERR_NOMEM;
    }
    box->refcnt = 1;
    box->cap = cap;
    out->owner = box;
    out->retain = zm_box_retain;
    out->release = zm_box_release;
    out->data = box->data;
    out->len = 0;
    out->cap = cap;
    return ZM_OK;
}

int zm_mem_dup(const void *data, size_t len, zm_mem *out)
{
    int rc;

    if (!out || (!data && len)) {
        return ZM_ERR_INVALID;
    }
    rc = zm_mem_alloc(len, out);
    if (rc != ZM_OK) {
        return rc;
    }
    if (len) {
        memcpy(out->data, data, len);
    }
    out->len = len;
    return ZM_OK;
}

int zm_mem_reserve(zm_mem *m, size_t cap)
{
    zm_mem next;
    int rc;
    size_t keep;

    if (!m) {
        return ZM_ERR_INVALID;
    }
    if (cap <= m->cap) {
        return ZM_OK;
    }
    rc = zm_mem_alloc(cap, &next);
    if (rc != ZM_OK) {
        return rc;
    }
    keep = m->len < cap ? m->len : cap;
    if (keep && m->data) {
        memcpy(next.data, m->data, keep);
    }
    next.len = keep;
    zm_mem_unref(m);
    *m = next;
    return ZM_OK;
}

void zm_mem_set_len(zm_mem *m, size_t len)
{
    if (!m) {
        return;
    }
    m->len = len < m->cap ? len : m->cap;
}
