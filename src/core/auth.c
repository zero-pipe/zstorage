/**
 * @file auth.c
 * @brief 推流/播放鉴权骨架：本地同步验签，不做网络 I/O。
 */
#include "zms/core/auth.h"
#include "zn/net/socket.h"
#include "zn/net/tcp_server.h"
#include "zn/util/log.h"
#include <stdio.h>
#include <string.h>

/* 进程启动前由 zms_auth_init()/zms_auth_set_verifier() 写，运行期只读。 */
static zms_auth_options g_opt;
static zms_auth_verify_fn g_verify;
static void *g_verify_user;
static int g_warned_no_verifier;

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(const char *in, size_t in_len, char *out, size_t cap)
{
    size_t oi = 0;

    for (size_t i = 0; i < in_len && oi + 1 < cap; ++i) {
        if (in[i] == '%' && i + 2 < in_len) {
            int hi = hex_val(in[i + 1]);
            int lo = hex_val(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out[oi++] = (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[oi++] = in[i] == '+' ? ' ' : in[i];
    }
    out[oi] = '\0';
}

int zms_auth_param_get(const char *params, const char *name, char *out, size_t cap)
{
    size_t nlen;
    const char *p;

    if (!out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!params || !name || !name[0]) {
        return -1;
    }
    nlen = strlen(name);
    p = params;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);

        if (seg > nlen && p[nlen] == '=' && strncmp(p, name, nlen) == 0) {
            url_decode(p + nlen + 1, seg - nlen - 1, out, cap);
            return 0;
        }
        if (!amp) {
            break;
        }
        p = amp + 1;
    }
    return -1;
}

void zms_auth_params_copy(const char *params, char *out, size_t cap)
{
    size_t n;

    if (!out || cap == 0) {
        return;
    }
    if (!params) {
        out[0] = '\0';
        return;
    }
    n = strlen(params);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, params, n);
    out[n] = '\0';
}

void zms_auth_split_query(const char *in, char *name, size_t name_cap, char *params,
                          size_t params_cap)
{
    const char *q;

    if (name && name_cap) {
        name[0] = '\0';
    }
    if (params && params_cap) {
        params[0] = '\0';
    }
    if (!in) {
        return;
    }
    q = strchr(in, '?');
    if (!q) {
        if (name && name_cap) {
            snprintf(name, name_cap, "%s", in);
        }
        return;
    }
    if (name && name_cap) {
        size_t n = (size_t)(q - in);

        if (n >= name_cap) {
            n = name_cap - 1;
        }
        memcpy(name, in, n);
        name[n] = '\0';
    }
    if (params && params_cap) {
        zms_auth_params_copy(q + 1, params, params_cap);
    }
}

void zms_auth_init(const zms_auth_options *opt)
{
    memset(&g_opt, 0, sizeof(g_opt));
    g_warned_no_verifier = 0;
    if (opt) {
        g_opt = *opt;
    }
    if (!g_opt.token_param[0]) {
        memcpy(g_opt.token_param, "token", sizeof("token"));
    }
    g_opt.token_param[sizeof(g_opt.token_param) - 1] = '\0';
    zn_info("[auth] token verify %s (param=%s)", g_opt.enable ? "enabled" : "disabled",
             g_opt.token_param);
}

void zms_auth_fini(void)
{
    memset(&g_opt, 0, sizeof(g_opt));
    g_verify = NULL;
    g_verify_user = NULL;
}

void zms_auth_set_verifier(zms_auth_verify_fn fn, void *user)
{
    g_verify = fn;
    g_verify_user = user;
}

const char *zms_auth_token_param(void)
{
    return g_opt.token_param[0] ? g_opt.token_param : "token";
}

void zms_auth_fill_peer(zn_tcp_session *tcp, char *ip, size_t cap, unsigned *port)
{
    zn_socket *sock;

    if (ip && cap) {
        ip[0] = '\0';
    }
    if (port) {
        *port = 0;
    }
    if (!tcp) {
        return;
    }
    sock = zn_tcp_session_socket(tcp);
    if (!sock) {
        return;
    }
    if (ip && cap) {
        uint16_t p = 0;

        (void)zn_socket_get_peer(sock, ip, cap, &p);
        if (port) {
            *port = p;
        }
    }
}

/* ── 内置 token 白名单（平台同步），用于未注册自定义验签实现时的本地比对 ─────── */

#define ZMS_AUTH_TOKEN_LIST_MAX 1024

static char g_tokens[ZMS_AUTH_TOKEN_LIST_MAX]; /* 逗号分隔的启用 token */
static volatile int g_tokens_ready;

void zms_auth_set_tokens(const char *csv)
{
    size_t n;

    if (!csv || !csv[0]) {
        g_tokens[0] = '\0';
        g_tokens_ready = 0;
        g_opt.enable = 0;
        zn_info("[auth] token list cleared, stream verify disabled");
        return;
    }
    n = strlen(csv);
    if (n >= sizeof(g_tokens)) {
        n = sizeof(g_tokens) - 1;
    }
    memcpy(g_tokens, csv, n);
    g_tokens[n] = '\0';
    g_opt.enable = 1;
    g_tokens_ready = 1;
    zn_info("[auth] token list updated (%zu bytes), stream verify enabled", n);
}

static int token_list_contains(const char *tok)
{
    const char *p = g_tokens;
    size_t tl;

    if (!tok || !tok[0]) {
        return 0;
    }
    tl = strlen(tok);
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t seg = comma ? (size_t)(comma - p) : strlen(p);
        const char *s = p;
        size_t sl = seg;

        while (sl && (*s == ' ' || *s == '\t')) {
            s++;
            sl--;
        }
        while (sl && (s[sl - 1] == ' ' || s[sl - 1] == '\t')) {
            sl--;
        }
        if (sl == tl && strncmp(s, tok, tl) == 0) {
            return 1;
        }
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return 0;
}

/* 内置验签：仅对外部“推流”强制 token；播放保持放行，避免影响平台自身观看链路。 */
static int builtin_verify(const zms_auth_request *req, zms_auth_decision *dec, void *user)
{
    char tok[256];

    (void)user;
    if (!dec) {
        return -1;
    }
    dec->allow = 0;
    dec->enable_mp4 = 0;
    if (req->action != ZMS_AUTH_ACTION_PUBLISH) {
        dec->allow = 1;
        return 0;
    }
    if (!req->params ||
        zms_auth_param_get(req->params, zms_auth_token_param(), tok, sizeof(tok)) != 0 ||
        !tok[0]) {
        dec->allow = 0;
        return 0;
    }
    dec->allow = token_list_contains(tok);
    return 0;
}

int zms_auth_check(const zms_auth_request *req, zms_auth_decision *dec)
{
    zms_auth_decision local;
    zms_auth_decision *out = dec ? dec : &local;

    out->allow = 1;
    out->enable_mp4 = 0;
    if (!req || !g_opt.enable) {
        return 1;
    }
    if (g_verify) {
        out->allow = 0;
        out->enable_mp4 = 0;
        if (g_verify(req, out, g_verify_user) != 0) {
            out->allow = 0;
            out->enable_mp4 = 0;
        }
        return out->allow ? 1 : 0;
    }
    if (g_tokens_ready) {
        if (builtin_verify(req, out, NULL) != 0) {
            out->allow = 0;
            out->enable_mp4 = 0;
        }
        return out->allow ? 1 : 0;
    }
    /* 启用了验签却既无实现又无白名单：fail-closed，只告警一次，避免刷日志。 */
    if (!g_warned_no_verifier) {
        zn_error("[auth] verify enabled but no verifier registered, deny all requests");
        g_warned_no_verifier = 1;
    }
    out->allow = 0;
    return 0;
}
