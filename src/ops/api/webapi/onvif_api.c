/**
 * @file onvif_api.c
 * @brief /index/api/onvif/* 实现：网络调用走线程池，回包回到 HTTP 会话 poller。
 *
 * 为什么必须异步：discover/probe/add/ptz 都要发 ONVIF SOAP（含 DNS、TCP、
 * 可能还有 HTTP Digest 挑战往返），这些是秒级阻塞操作。如果在 HTTP poller
 * 线程上同步跑，一个慢设备就能卡死该 poller 上所有连接的收发。
 *
 * 两段式执行：
 *   - job_run 在**线程池工作线程**上跑网络 IO，把结果写进 job 结构；
 *   - job_on_io 被投递回**原 HTTP 会话的 poller**上执行，此时才生成 JSON、
 *     发回包并释放 session（HTTP 会话 API 不是线程安全的，必须在 poller 上）。
 * 两段之间用 zcs_http_session_async_hold/release 保活会话，防止客户端中途断开。
 */
#include "ops/api/webapi/onvif_api.h"
#include "zcs/core/stream/limits.h"
#include "zcs/device/onvif/onvif.h"
#include "zcs/ops/config/config.h"
#include "zcs/device/onvif/onvif_registry.h"
#include "zcs/util/thread_pool.h"
#include "zcs/protocol/http/session_api.h"
#include "zn/net/tcp_server.h"
#include "zn/util/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ONVIF_JOB_BODY_MAX 16384

typedef enum {
    ONVIF_K_DISCOVER = 0,
    ONVIF_K_PROBE,
    ONVIF_K_ADD,
    ONVIF_K_PTZ,
} onvif_kind;

typedef struct onvif_job {
    zcs_http_session *hs;
    zn_tcp_session *tcp;
    zn_poller *poller;
    zn_poller_pool *pool;
    int kind;
    char addr[ZCS_ONVIF_XADDR_MAX];
    char user[64];
    char pass[128];
    char profile[64];
    char vhost[64];
    char app[64];
    char stream[64];
    int timeout_ms;
    float x;
    float y;
    float z;
    int stop;
    /* 结果 */
    int status;
    char err[128];
    zcs_onvif_device dev;
    zcs_onvif_device devs[16];
    size_t dev_count;
    zcs_onvif_profile profiles[8];
    size_t profile_count;
    char key[256];
    char uri[ZCS_ONVIF_URI_MAX];
} onvif_job;

/* ── JSON ───────────────────────────────────────────────────────────────── */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    /** 1 = 发生过截断；JSON 不完整时改回错误响应，不能发半截 JSON */
    int overflow;
} jw_t;

static void jw_init(jw_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
    if (cap) {
        buf[0] = '\0';
    }
}

static void jw_putc(jw_t *w, char c)
{
    if (w->len + 1 < w->cap) {
        w->buf[w->len++] = c;
        w->buf[w->len] = '\0';
    } else {
        w->overflow = 1;
    }
}

static void jw_puts(jw_t *w, const char *s)
{
    while (s && *s) {
        jw_putc(w, *s++);
    }
}

static void jw_fmt(jw_t *w, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (w->len + 1 >= w->cap) {
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(w->buf + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    /* vsnprintf 返回“本该写入的长度”：大于剩余空间说明被截断，此时把
     * len 钳到 cap-1 并置 overflow。调用方最终据此改用错误 JSON，
     * 绝不能把半截 JSON 发给客户端（半截 JSON 在前端会解析失败）。 */
    if ((size_t)n >= w->cap - w->len) {
        w->len = w->cap - 1;
        w->overflow = 1;
    } else {
        w->len += (size_t)n;
    }
}

static void jw_esc(jw_t *w, const char *s)
{
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        switch (*p) {
        case '"':
            jw_puts(w, "\\\"");
            break;
        case '\\':
            jw_puts(w, "\\\\");
            break;
        case '\n':
            jw_puts(w, "\\n");
            break;
        case '\r':
            jw_puts(w, "\\r");
            break;
        case '\t':
            jw_puts(w, "\\t");
            break;
        default:
            if (*p < 0x20) {
                jw_fmt(w, "\\u%04x", *p);
            } else {
                jw_putc(w, (char)*p);
            }
            break;
        }
        ++p;
    }
}

static void jw_kv_str(jw_t *w, const char *key, const char *val)
{
    jw_fmt(w, "\"%s\":\"", key);
    jw_esc(w, val);
    jw_puts(w, "\"");
}

/* ── job ────────────────────────────────────────────────────────────────── */

static void job_fail(onvif_job *j, const char *msg)
{
    j->status = -1;
    snprintf(j->err, sizeof(j->err), "%s", msg ? msg : "failed");
}

static void stream_sanitize(const char *in, char *out, size_t cap)
{
    size_t i;
    for (i = 0; in && in[i] && i + 1 < cap; ++i) {
        char c = in[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
            c == '-') {
            out[i] = c;
        } else {
            out[i] = '_';
        }
    }
    out[i] = '\0';
}

static void job_run(onvif_job *j)
{
    if (j->kind == ONVIF_K_DISCOVER) {
        size_t n = 0;
        if (zcs_onvif_discover(j->timeout_ms, j->devs, 16, &n) != 0) {
            job_fail(j, "discover failed");
            return;
        }
        j->dev_count = n;
        j->status = 0;
        return;
    }

    {
        /* 除 discover 外都先建 ONVIF 客户端（内部完成 SOAP 认证与设备信息
         * 拉取）。连接失败统一走 job_fail，job 里只留错误串供回包使用。 */
        zcs_onvif_client *c = zcs_onvif_connect(j->addr, j->user[0] ? j->user : NULL,
                                                j->pass[0] ? j->pass : NULL, j->timeout_ms);
        if (!c) {
            job_fail(j, "connect failed");
            return;
        }
        if (j->kind == ONVIF_K_PTZ) {
            const zcs_onvif_device *info = zcs_onvif_device_info(c);
            /* 设备可能没有 PTZ 服务：直接报错比发一个注定失败的 SOAP 更清晰。 */
            if (!info || info->ptz_xaddr[0] == '\0') {
                job_fail(j, "device has no PTZ service");
            } else {
                int rc = j->stop ? zcs_onvif_ptz_stop(c, j->profile)
                                 : zcs_onvif_ptz_move(c, j->profile, j->x, j->y, j->z);
                if (rc != 0) {
                    job_fail(j, "ptz failed");
                }
            }
        } else {
            const zcs_onvif_device *info = zcs_onvif_device_info(c);
            if (info) {
                j->dev = *info;
            }
            if (zcs_onvif_get_profiles(c, j->profiles, sizeof(j->profiles) / sizeof(j->profiles[0]),
                                       &j->profile_count) != 0) {
                job_fail(j, "get profiles failed");
            }
        }
        zcs_onvif_close(c);
    }

    /* ADD 的收尾（仍在线程池里做，未触碰任何 HTTP/会话状态）：
     * 在已取到的 profile 列表里定位用户指定/默认的那条，取出拉流 URI，
     * 并补齐 stream/app 的缺省值。真正“登记为拉流代理”放到 job_apply，
     * 因为那会创建代理对象并挂到共享 registry，必须在 IO 线程串行执行。 */
    if (j->kind == ONVIF_K_ADD && j->status == 0) {
        size_t i;
        size_t chosen = 0;
        if (j->profile_count == 0) {
            job_fail(j, "no profile");
            return;
        }
        if (j->profile[0]) {
            int found = 0;
            for (i = 0; i < j->profile_count; ++i) {
                if (strcmp(j->profiles[i].token, j->profile) == 0) {
                    chosen = i;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                job_fail(j, "profile not found");
                return;
            }
        }
        snprintf(j->profile, sizeof(j->profile), "%s", j->profiles[chosen].token);
        if (j->profiles[chosen].uri[0] == '\0') {
            job_fail(j, "no stream uri");
            return;
        }
        snprintf(j->uri, sizeof(j->uri), "%s", j->profiles[chosen].uri);
        /* 未指定流名时用 profile token 消毒后当流名（token 可能带非法字符，
         * stream_sanitize 把非 [A-Za-z0-9_-] 换成下划线，保证 URL 安全）。 */
        if (j->stream[0] == '\0') {
            char tmp[ZCS_STREAM_MAX];
            stream_sanitize(j->profile, tmp, sizeof(tmp));
            snprintf(j->stream, sizeof(j->stream), "%s", tmp);
        }
        if (j->app[0] == '\0') {
            snprintf(j->app, sizeof(j->app), "onvif");
        }
    }
}

/** 只在 IO 线程执行：设备登记（创建拉流代理） */
static void job_apply(onvif_job *j)
{
    if (j->kind != ONVIF_K_ADD || j->status != 0) {
        return;
    }
    /* registry_add 会创建 live_pull_proxy 并加入全局 registry，属于会与
     * 其它会话共享的状态变更，因此挪到 poller 线程（job_on_io）里做，
     * 而不是线程池工作线程。失败则把 job 标成失败，回包走错误 JSON。 */
    if (zcs_onvif_registry_add(j->addr, j->user, j->pass, j->profile, j->uri,
                               j->vhost[0] ? j->vhost : NULL, j->app, j->stream, j->pool, j->key,
                               sizeof(j->key)) != 0) {
        job_fail(j, "add pull proxy failed");
    }
}

static size_t job_build_body(onvif_job *j, char *body, size_t cap)
{
    jw_t w;
    jw_init(&w, body, cap);

    if (j->status != 0) {
        jw_puts(&w, "{\"code\":-1,");
        jw_kv_str(&w, "msg", j->err[0] ? j->err : "failed");
        jw_puts(&w, "}");
        return w.len;
    }

    switch (j->kind) {
    case ONVIF_K_DISCOVER: {
        size_t i;
        jw_puts(&w, "{\"code\":0,\"data\":[");
        for (i = 0; i < j->dev_count; ++i) {
            const zcs_onvif_device *d = &j->devs[i];
            if (i) {
                jw_putc(&w, ',');
            }
            jw_putc(&w, '{');
            jw_kv_str(&w, "xaddr", d->xaddr);
            jw_putc(&w, ',');
            jw_kv_str(&w, "uuid", d->uuid);
            jw_putc(&w, ',');
            jw_kv_str(&w, "name", d->name);
            jw_putc(&w, ',');
            jw_kv_str(&w, "scopes", d->scopes);
            jw_putc(&w, '}');
        }
        jw_puts(&w, "]}");
        break;
    }
    case ONVIF_K_PROBE:
    case ONVIF_K_ADD: {
        size_t i;
        const zcs_onvif_device *d = &j->dev;
        jw_puts(&w, "{\"code\":0,\"data\":{");
        jw_kv_str(&w, "xaddr", d->xaddr);
        jw_putc(&w, ',');
        jw_kv_str(&w, "manufacturer", d->manufacturer);
        jw_putc(&w, ',');
        jw_kv_str(&w, "model", d->model);
        jw_putc(&w, ',');
        jw_kv_str(&w, "firmware", d->firmware);
        jw_putc(&w, ',');
        jw_kv_str(&w, "serial", d->serial);
        jw_putc(&w, ',');
        jw_kv_str(&w, "media", d->media_xaddr);
        jw_putc(&w, ',');
        jw_kv_str(&w, "ptz", d->ptz_xaddr);
        jw_puts(&w, ",\"profiles\":[");
        for (i = 0; i < j->profile_count; ++i) {
            const zcs_onvif_profile *p = &j->profiles[i];
            if (i) {
                jw_putc(&w, ',');
            }
            jw_putc(&w, '{');
            jw_kv_str(&w, "token", p->token);
            jw_putc(&w, ',');
            jw_kv_str(&w, "name", p->name);
            jw_putc(&w, ',');
            jw_kv_str(&w, "encoding", p->encoding);
            jw_fmt(&w, ",\"width\":%d,\"height\":%d,\"fps\":%d,", p->width, p->height, p->fps);
            jw_kv_str(&w, "uri", p->uri);
            jw_putc(&w, '}');
        }
        jw_puts(&w, "]");
        if (j->kind == ONVIF_K_ADD) {
            jw_putc(&w, ',');
            jw_kv_str(&w, "key", j->key);
            jw_putc(&w, ',');
            jw_kv_str(&w, "profile", j->profile);
            jw_putc(&w, ',');
            jw_kv_str(&w, "stream", j->stream);
        }
        jw_puts(&w, "}}");
        break;
    }
    case ONVIF_K_PTZ:
        jw_puts(&w, "{\"code\":0,\"msg\":\"success\"}");
        break;
    default:
        jw_puts(&w, "{\"code\":-1,\"msg\":\"unsupported\"}");
        break;
    }
    if (w.overflow) {
        /* 响应超限：返回明确的错误 JSON，绝不发送半截 JSON */
        static const char err[] = "{\"code\":-1,\"msg\":\"response too large\"}";
        size_t n = sizeof(err) - 1;
        if (n >= cap) {
            n = cap - 1;
        }
        memcpy(body, err, n);
        body[n] = '\0';
        return n;
    }
    return w.len;
}

static int sess_alive(const onvif_job *j)
{
    return j && zcs_http_session_is_live(j->hs);
}

static void job_work(void *user)
{
    onvif_job *j = (onvif_job *)user;
    job_run(j);
}

static void job_on_io(void *user)
{
    onvif_job *j = (onvif_job *)user;
    /* 响应体 16KB：堆上拼，别占 poller 栈。 */
    char *body = (char *)malloc(ONVIF_JOB_BODY_MAX);
    size_t len;

    if (!j) {
        return;
    }
    if (!body) {
        zcs_http_response_send_error(j->hs, 500, "Internal Server Error");
        zcs_http_session_async_release(j->hs);
        free(j);
        return;
    }
    /* 客户端可能已在异步期间断开：hold 只是延缓释放，会话仍可能被标记
     * 为非 live。此时不能尝试回包（socket 已不可用），直接释放收工。 */
    if (!sess_alive(j)) {
        zcs_http_session_async_release(j->hs);
        free(body);
        free(j);
        return;
    }
    /* 顺序很重要：先 apply（可能把 job 标失败），再根据最终 status 生成
     * body。反过来会把 apply 的失败吞掉，回一个假成功。 */
    job_apply(j);
    len = job_build_body(j, body, ONVIF_JOB_BODY_MAX);
    zcs_http_response_send_json(j->hs, 200, body, len);
    zcs_http_session_async_release(j->hs);
    free(body);
    free(j);
}

/* ── 路由入口 ───────────────────────────────────────────────────────────── */

static int onvif_kind_of(const char *endpoint, int *kind)
{
    if (strcmp(endpoint, "discover") == 0) {
        *kind = ONVIF_K_DISCOVER;
    } else if (strcmp(endpoint, "probe") == 0) {
        *kind = ONVIF_K_PROBE;
    } else if (strcmp(endpoint, "add") == 0) {
        *kind = ONVIF_K_ADD;
    } else if (strcmp(endpoint, "ptz") == 0) {
        *kind = ONVIF_K_PTZ;
    } else {
        return -1;
    }
    return 0;
}

typedef struct {
    jw_t *w;
    int first;
} list_ctx;

static int list_visit(const zcs_onvif_entry_info *info, void *user)
{
    list_ctx *ctx = (list_ctx *)user;
    jw_t *w = ctx->w;
    if (!ctx->first) {
        jw_putc(w, ',');
    }
    ctx->first = 0;
    jw_putc(w, '{');
    jw_kv_str(w, "key", info->key);
    jw_putc(w, ',');
    jw_kv_str(w, "addr", info->addr);
    jw_putc(w, ',');
    jw_kv_str(w, "profile", info->profile);
    jw_putc(w, ',');
    jw_kv_str(w, "uri", info->uri);
    jw_putc(w, ',');
    jw_kv_str(w, "app", info->app);
    jw_putc(w, ',');
    jw_kv_str(w, "stream", info->stream);
    jw_fmt(w, ",\"online\":%s", info->online ? "true" : "false");
    jw_putc(w, '}');
    return 0;
}

static size_t handle_list_real(char *body, size_t cap)
{
    jw_t w;
    list_ctx ctx;
    jw_init(&w, body, cap);
    jw_puts(&w, "{\"code\":0,\"data\":[");
    ctx.w = &w;
    ctx.first = 1;
    zcs_onvif_registry_foreach(list_visit, &ctx);
    jw_puts(&w, "]}");
    return w.len;
}

/* ONVIF API 总入口。同步端点（list/del）当场写好 body 返回；
 * 异步端点（discover/probe/add/ptz）提交到线程池后立即返回。
 *
 * @return 1 = 已同步填好 body（*http_status 有效，调用方直接回包）
 *         0 = 已转异步（后续由 job_on_io 回包，调用方不要再回）
 *        -1 = 非本模块端点（交回上层继续路由）
 *  这也是为什么异步分支里 *http_status=0 且 return 0：告诉调用方“别回包”。 */
int zcs_webapi_onvif_handle(zn_tcp_session *tcp, const zcs_web_api_opts *opts,
                            const zcs_onvif_api_req *req, int *http_status, char *body,
                            size_t body_cap)
{
    onvif_job *job;
    int kind;
    const zcs_config *cfg = opts ? opts->cfg : NULL;

    if (!req || !req->endpoint || !http_status || !body) {
        return -1;
    }
    *http_status = 200;

    if (strcmp(req->endpoint, "list") == 0) {
        (void)handle_list_real(body, body_cap);
        return 1;
    }
    if (strcmp(req->endpoint, "del") == 0) {
        int removed = 0;
        if (req->key && req->key[0]) {
            removed = zcs_onvif_registry_del_key(req->key);
        } else if (req->addr && req->addr[0]) {
            (void)zcs_onvif_registry_del_addr(req->addr, &removed);
        } else {
            snprintf(body, body_cap, "{\"code\":-1,\"msg\":\"missing key/addr\"}");
            *http_status = 200;
            return 1;
        }
        snprintf(body, body_cap, "{\"code\":0,\"data\":{\"removed\":%d}}", removed);
        return 1;
    }
    if (onvif_kind_of(req->endpoint, &kind) != 0) {
        return -1;
    }
    if (!opts || !opts->poller_pool) {
        snprintf(body, body_cap, "{\"code\":-1,\"msg\":\"no poller pool\"}");
        return 1;
    }
    if ((kind == ONVIF_K_PROBE || kind == ONVIF_K_ADD || kind == ONVIF_K_PTZ) &&
        (!req->addr || !req->addr[0])) {
        snprintf(body, body_cap, "{\"code\":-1,\"msg\":\"missing addr\"}");
        return 1;
    }
    if (kind == ONVIF_K_PTZ && (!req->profile || !req->profile[0])) {
        snprintf(body, body_cap, "{\"code\":-1,\"msg\":\"missing profile\"}");
        return 1;
    }

    job = (onvif_job *)calloc(1, sizeof(*job));
    if (!job) {
        snprintf(body, body_cap, "{\"code\":-1,\"msg\":\"oom\"}");
        return 1;
    }
    job->kind = kind;
    job->hs = (zcs_http_session *)zn_tcp_session_user(tcp);
    job->tcp = tcp;
    job->poller = opts->poller;
    job->pool = opts->poller_pool;
    if (req->addr) {
        snprintf(job->addr, sizeof(job->addr), "%s", req->addr);
    }
    if (req->user && req->user[0]) {
        snprintf(job->user, sizeof(job->user), "%s", req->user);
    } else if (cfg) {
        snprintf(job->user, sizeof(job->user), "%s", cfg->onvif.auth_user);
    }
    if (req->pass && req->pass[0]) {
        snprintf(job->pass, sizeof(job->pass), "%s", req->pass);
    } else if (cfg) {
        snprintf(job->pass, sizeof(job->pass), "%s", cfg->onvif.auth_pass);
    }
    if (req->profile) {
        snprintf(job->profile, sizeof(job->profile), "%s", req->profile);
    }
    if (req->vhost && req->vhost[0]) {
        snprintf(job->vhost, sizeof(job->vhost), "%s", req->vhost);
    } else if (cfg) {
        snprintf(job->vhost, sizeof(job->vhost), "%s", cfg->onvif.default_vhost);
    }
    if (req->app && req->app[0]) {
        snprintf(job->app, sizeof(job->app), "%s", req->app);
    } else if (cfg) {
        snprintf(job->app, sizeof(job->app), "%s", cfg->onvif.default_app);
    }
    if (req->stream) {
        snprintf(job->stream, sizeof(job->stream), "%s", req->stream);
    }
    job->x = req->x;
    job->y = req->y;
    job->z = req->z;
    job->stop = req->stop;
    job->timeout_ms = req->timeout_ms > 0 ? req->timeout_ms
                                          : (cfg && cfg->onvif.timeout_ms > 0 ? cfg->onvif.timeout_ms
                                                                              : 5000);

    if (!job->hs || !zcs_vod_thread_pool_enabled() || !opts->poller) {
        /* 没有异步执行环境就明确失败：ONVIF 调用含阻塞 DNS/HTTP，
         * 绝不能在 HTTP poller 上同步跑。 */
        free(job);
        *http_status = 503;
        return (int)snprintf(body, body_cap,
                             "{\"code\":-1,\"msg\":\"async io unavailable\"}");
    }

    zcs_http_session_async_hold(job->hs);
    if (zcs_vod_thread_pool_run(opts->poller, job_work, job_on_io, job) != ZN_OK) {
        size_t len;
        zn_warn("onvif: submit job failed");
        zcs_http_session_async_release(job->hs);
        free(job);
        *http_status = 503;
        len = (size_t)snprintf(body, body_cap,
                               "{\"code\":-1,\"msg\":\"async io unavailable\"}");
        /* 提交失败退回同步回包（503），返回 1 表示 body 已就绪。 */
        return (int)(len > 0);
    }
    *http_status = 0; /* 异步：稍后由 on_io 回包 */
    return 0;
}
