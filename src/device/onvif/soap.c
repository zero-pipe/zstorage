/**
 * @file soap.c
 * @brief 极简 SOAP 1.2 / XML 层 + ONVIF 鉴权（WS-UsernameToken / Digest / Basic）。
 */
#include "device/onvif/onvif_internal.h"
#include "zcs/util/md5.h"
#include "zcs/util/sha1.h"
#include "zn/util/log.h"
#include "zcs/util/base64.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define strncasecmp _strnicmp
#endif

/* ── 通用小工具 ─────────────────────────────────────────────────────────── */

static unsigned g_rand_seeded;

static unsigned onvif_rand(void)
{
    if (!g_rand_seeded) {
        g_rand_seeded = 1;
        srand((unsigned)time(NULL) ^ 0x9e3779b9u);
    }
    return (unsigned)rand();
}

static void rand_bytes(uint8_t *out, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        out[i] = (uint8_t)(onvif_rand() & 0xff);
    }
}

void zcs_onvif_make_msgid(char *out, size_t cap)
{
    uint8_t r[16];
    rand_bytes(r, sizeof(r));
    snprintf(out, cap,
             "uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12],
             r[13], r[14], r[15]);
}

void zcs_onvif_utc_now(char *out, size_t cap)
{
    time_t t = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

size_t zcs_onvif_base64(const uint8_t *data, size_t len, char *out, size_t cap)
{
    size_t n = 0;

    /* ONVIF 内部约定：容量不足返回 0（调用方已按此判错）。 */
    if (zcs_base64_encode(data, len, out, cap, &n) != ZN_OK) {
        return 0;
    }
    return n;
}

/* ── SOAP 构建 ──────────────────────────────────────────────────────────── */

static int b64_sha1_digest(const uint8_t *nonce, size_t nonce_len, const char *created,
                           const char *pass, char *out, size_t cap)
{
    zcs_sha1_ctx ctx;
    uint8_t digest[20];

    zcs_sha1_init(&ctx);
    zcs_sha1_update(&ctx, nonce, nonce_len);
    zcs_sha1_update(&ctx, created, strlen(created));
    zcs_sha1_update(&ctx, pass, strlen(pass));
    zcs_sha1_final(&ctx, digest);
    return zcs_onvif_base64(digest, sizeof(digest), out, cap) > 0 ? 0 : -1;
}

size_t zcs_onvif_soap_build(char *out, size_t cap, const char *body_inner, const char *user,
                            const char *pass)
{
    char msgid[80];
    int n;

    if (!out || cap == 0 || !body_inner) {
        return 0;
    }
    zcs_onvif_make_msgid(msgid, sizeof(msgid));

    n = snprintf(
        out, cap,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\""
        " xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\""
        " xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\""
        " xmlns:tptz=\"http://www.onvif.org/ver20/ptz/wsdl\""
        " xmlns:tt=\"http://www.onvif.org/ver10/schema\""
        " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\""
        " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
        "<s:Header>");
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    if (user && user[0] && pass && pass[0]) {
        uint8_t nonce[16];
        char nonce_b64[64];
        char passwd_b64[64];
        char created[40];

        rand_bytes(nonce, sizeof(nonce));
        zcs_onvif_utc_now(created, sizeof(created));
        if (zcs_onvif_base64(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64)) == 0 ||
            b64_sha1_digest(nonce, sizeof(nonce), created, pass, passwd_b64, sizeof(passwd_b64)) !=
                0) {
            return 0;
        }
        {
            int m = snprintf(
                out + n, cap - (size_t)n,
                "<wsse:Security s:mustUnderstand=\"true\">"
                "<wsse:UsernameToken>"
                "<wsse:Username>%s</wsse:Username>"
                "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
                "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</wsse:Password>"
                "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
                "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%s</wsse:Nonce>"
                "<wsu:Created>%s</wsu:Created>"
                "</wsse:UsernameToken>"
                "</wsse:Security>",
                user, passwd_b64, nonce_b64, created);
            if (m < 0 || (size_t)m >= cap - (size_t)n) {
                return 0;
            }
            n += m;
        }
    }
    {
        int m = snprintf(out + n, cap - (size_t)n, "</s:Header><s:Body>%s</s:Body></s:Envelope>",
                         body_inner);
        if (m < 0 || (size_t)m >= cap - (size_t)n) {
            return 0;
        }
        n += m;
    }
    return (size_t)n;
}

/* ── HTTP / 鉴权 ────────────────────────────────────────────────────────── */

static int url_path(const char *url, char *out, size_t cap)
{
    const char *p = strstr(url, "://");
    if (!p) {
        return -1;
    }
    p = strchr(p + 3, '/');
    if (!p) {
        snprintf(out, cap, "/");
        return 0;
    }
    snprintf(out, cap, "%s", p);
    return 0;
}

static int header_value(const char *headers, const char *name, char *out, size_t cap)
{
    const char *line = headers;
    size_t name_len = strlen(name);

    out[0] = '\0';
    while (line && *line) {
        const char *eol = strstr(line, "\r\n");
        size_t len;
        if (!eol) {
            break;
        }
        len = (size_t)(eol - line);
        if (len > name_len && line[name_len] == ':' &&
            strncasecmp(line, name, name_len) == 0) {
            const char *v = line + name_len + 1;
            size_t vlen = (size_t)(eol - v);
            while (vlen > 0 && (*v == ' ' || *v == '\t')) {
                ++v;
                --vlen;
            }
            if (vlen >= cap) {
                vlen = cap - 1;
            }
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 0;
        }
        line = eol + 2;
    }
    return -1;
}

static int digest_param(const char *challenge, const char *key, char *out, size_t cap)
{
    char pat[64];
    const char *p;

    snprintf(pat, sizeof(pat), "%s=", key);
    p = strstr(challenge, pat);
    if (!p) {
        return -1;
    }
    p += strlen(pat);
    if (*p == '"') {
        const char *end = strchr(p + 1, '"');
        size_t len;
        if (!end) {
            return -1;
        }
        len = (size_t)(end - p - 1);
        if (len >= cap) {
            len = cap - 1;
        }
        memcpy(out, p + 1, len);
        out[len] = '\0';
    } else {
        size_t len = strcspn(p, ", ");
        if (len >= cap) {
            len = cap - 1;
        }
        memcpy(out, p, len);
        out[len] = '\0';
    }
    return 0;
}

static int build_digest_header(const char *user, const char *pass, const char *challenge,
                               const char *uri, char *out, size_t cap)
{
    char realm[128];
    char nonce[128];
    char qop[64];
    char opaque[128];
    char cnonce[40];
    char ha1[33];
    char ha2[33];
    char response[33];
    char a1[256];
    char a2[512];
    char resp_src[512];
    char opaque_part[200];

    if (digest_param(challenge, "realm", realm, sizeof(realm)) != 0 ||
        digest_param(challenge, "nonce", nonce, sizeof(nonce)) != 0) {
        return -1;
    }
    if (digest_param(challenge, "qop", qop, sizeof(qop)) != 0) {
        qop[0] = '\0';
    }
    if (digest_param(challenge, "opaque", opaque, sizeof(opaque)) != 0) {
        opaque[0] = '\0';
    }
    snprintf(cnonce, sizeof(cnonce), "%08x%08x", onvif_rand(), onvif_rand());

    snprintf(a1, sizeof(a1), "%s:%s:%s", user, realm, pass);
    zcs_md5_hex_str(a1, ha1);
    snprintf(a2, sizeof(a2), "POST:%s", uri);
    zcs_md5_hex_str(a2, ha2);

    if (qop[0] && strstr(qop, "auth")) {
        snprintf(resp_src, sizeof(resp_src), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
    } else {
        snprintf(resp_src, sizeof(resp_src), "%s:%s:%s", ha1, nonce, ha2);
    }
    zcs_md5_hex_str(resp_src, response);

    if (opaque[0]) {
        snprintf(opaque_part, sizeof(opaque_part), ", opaque=\"%s\"", opaque);
    } else {
        opaque_part[0] = '\0';
    }
    if (qop[0] && strstr(qop, "auth")) {
        int n = snprintf(out, cap,
                         "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                         "uri=\"%s\", qop=auth, nc=00000001, cnonce=\"%s\", response=\"%s\"%s\r\n",
                         user, realm, nonce, uri, cnonce, response, opaque_part);
        return (n < 0 || (size_t)n >= cap) ? -1 : 0;
    }
    {
        int n = snprintf(out, cap,
                         "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", "
                         "uri=\"%s\", response=\"%s\"%s\r\n",
                         user, realm, nonce, uri, response, opaque_part);
        return (n < 0 || (size_t)n >= cap) ? -1 : 0;
    }
}

static int soap_post_once(zcs_onvif_client *c, const char *xaddr, const char *soap,
                          const char *ctype, const char *extra_headers, zcs_http_resp *resp)
{
    zcs_http_req req;
    memset(&req, 0, sizeof(req));
    req.url = xaddr;
    req.method = "POST";
    req.content_type = ctype;
    req.body = soap;
    req.body_len = strlen(soap);
    req.extra_headers = extra_headers;
    req.timeout_ms = c ? c->timeout_ms : 10000;
    return zcs_http_exec(&req, resp);
}

static void fault_reason(const char *body, char *out, size_t cap)
{
    const char *p;
    const char *end;
    size_t n;

    out[0] = '\0';
    if (!body) {
        return;
    }
    p = strstr(body, "Reason");
    if (!p) {
        return;
    }
    p = strstr(p, "Text");
    if (!p) {
        return;
    }
    p = strchr(p, '>');
    if (!p) {
        return;
    }
    ++p;
    end = strchr(p, '<');
    n = end ? (size_t)(end - p) : strlen(p);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

int zcs_onvif_call(zcs_onvif_client *c, const char *xaddr, const char *body_inner, char **out_body)
{
    /* SOAP 报文在堆上构建：64KB 量级不能放在线程栈上（ONVIF 客户端跑在
     * vod worker，同一份代码也可能被 API 线程调用）。 */
    char *soap = NULL;
    char challenge[ZCS_ONVIF_CHALLENGE_MAX];
    const char *ctype = "application/soap+xml; charset=utf-8";
    int attempts = 0;
    int xml_fallback_used = 0;
    int probe_plain = 0;
    int have_creds;
    int use_plain;
    int ret = -1;

    if (!c || !xaddr || !body_inner || !out_body) {
        return -1;
    }
    *out_body = NULL;
    challenge[0] = 0;
    have_creds = c->user[0] && c->pass[0];
    soap = (char *)malloc(ZCS_ONVIF_BODY_MAX);
    if (!soap) {
        return -1;
    }
    /* 已确认设备走 HTTP Digest：先用无 Security 的最小请求换取挑战，减少失败登录计数 */
    use_plain = (c->auth_mode == 1) || !have_creds;

    while (attempts++ < 4) {
        char extra[ZCS_ONVIF_EXTRA_HDR_MAX];
        zcs_http_resp resp;
        int rc;

        /* 每次尝试只构建当前需要的报文（带/不带 WS-Security）。 */
        if (zcs_onvif_soap_build(soap, ZCS_ONVIF_BODY_MAX, body_inner,
                                 use_plain ? NULL : (have_creds ? c->user : NULL),
                                 use_plain ? NULL : (have_creds ? c->pass : NULL)) == 0) {
            zn_warn("onvif: build soap failed (%s)", xaddr);
            goto done;
        }
        extra[0] = 0;
        if (challenge[0] && have_creds) {
            char auth[ZCS_ONVIF_AUTH_HDR_MAX];
            char uri[256];
            int auth_ok = 0;

            auth[0] = 0;
            if (url_path(xaddr, uri, sizeof(uri)) != 0) {
                snprintf(uri, sizeof(uri), "/onvif/device_service");
            }
            if (strstr(challenge, "Digest") != NULL) {
                auth_ok = build_digest_header(c->user, c->pass, challenge, uri, auth,
                                              sizeof(auth)) == 0;
            } else {
                char cred[ZCS_ONVIF_USER_MAX + ZCS_ONVIF_PASS_MAX + 2];
                char b64[ZCS_ONVIF_AUTH_HDR_MAX];
                snprintf(cred, sizeof(cred), "%s:%s", c->user, c->pass);
                if (zcs_onvif_base64((const uint8_t *)cred, strlen(cred), b64, sizeof(b64)) > 0) {
                    int n = snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", b64);
                    auth_ok = n > 0 && (size_t)n < sizeof(auth);
                }
            }
            if (!auth_ok) {
                zn_warn("onvif: build auth header failed (%s)", xaddr);
                goto done;
            }
            snprintf(extra, sizeof(extra), "%s", auth);
        }

        memset(&resp, 0, sizeof(resp));
        rc = soap_post_once(c, xaddr, soap, ctype, extra[0] ? extra : NULL, &resp);
        if (rc != 0) {
            zcs_http_resp_free(&resp);
            goto done;
        }
        if (resp.status == 200) {
            if (challenge[0]) {
                c->auth_mode = 1;
            }
            *out_body = resp.body;
            resp.body = NULL;
            zcs_http_resp_free(&resp);
            ret = 0;
            goto done;
        }
        if (resp.status == 401) {
            if (!challenge[0] && resp.headers) {
                (void)header_value(resp.headers, "WWW-Authenticate", challenge, sizeof(challenge));
            }
            zcs_http_resp_free(&resp);
            if (challenge[0] && have_creds) {
                continue;
            }
            zn_warn("onvif: %s unauthorized (no challenge/creds)", xaddr);
            goto done;
        }
        if (resp.status == 400) {
            int is_fault = resp.body && strstr(resp.body, "Fault") != NULL;
            if (is_fault && have_creds && !challenge[0] && !probe_plain) {
                /* 设备要求 HTTP 鉴权但未给挑战（如海康 ter:NotAuthorized）：
                 * 用无 Security 的请求换取 401 挑战，再带 Digest 重试。 */
                probe_plain = 1;
                use_plain = 1;
                zcs_http_resp_free(&resp);
                continue;
            }
            if (is_fault) {
                char reason[200];
                fault_reason(resp.body, reason, sizeof(reason));
                zn_warn("onvif: %s -> HTTP %d (%s)", xaddr, resp.status,
                        reason[0] ? reason : "soap fault");
                zcs_http_resp_free(&resp);
                goto done;
            }
            if (!xml_fallback_used) {
                xml_fallback_used = 1;
                ctype = "text/xml; charset=utf-8";
                zcs_http_resp_free(&resp);
                continue;
            }
        }
        if (resp.status == 415 && !xml_fallback_used) {
            xml_fallback_used = 1;
            ctype = "text/xml; charset=utf-8";
            zcs_http_resp_free(&resp);
            continue;
        }
        zn_warn("onvif: %s -> HTTP %d", xaddr, resp.status);
        zcs_http_resp_free(&resp);
        goto done;
    }
done:
    free(soap);
    return ret;
}
