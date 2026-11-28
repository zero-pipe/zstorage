/**
 * @file test_auth_token.c
 * @brief 推流/播放 token 验签骨架：默认放行、fail-closed、参数解析与结论透传。
 */
#include "zcs/core/auth.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static char g_seen_params[ZCS_AUTH_PARAMS_MAX];
static int g_seen_action = -1;
static int g_verify_calls;

/* 测试用验签实现：params 里 token=good 即放行。 */
static int test_verify(const zcs_auth_request *req, zcs_auth_decision *dec, void *user)
{
    char token[64];

    (void)user;
    ++g_verify_calls;
    g_seen_action = (int)req->action;
    zcs_auth_params_copy(req->params, g_seen_params, sizeof(g_seen_params));
    dec->allow = 0;
    dec->enable_mp4 = 0;
    if (zcs_auth_param_get(req->params, "token", token, sizeof(token)) == 0 &&
        strcmp(token, "good") == 0) {
        dec->allow = 1;
        dec->enable_mp4 = 1;
        return 0;
    }
    return 0;
}

static int test_params(void)
{
    char out[64];

    CHECK(zcs_auth_param_get("a=1&token=abc&b=2", "token", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "abc") == 0);
    /* URL 解码 */
    CHECK(zcs_auth_param_get("token=a%2Bb%20c", "token", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "a+b c") == 0);
    /* 没有匹配键 */
    CHECK(zcs_auth_param_get("tokenx=1&tok=2", "token", out, sizeof(out)) != 0);
    CHECK(out[0] == '\0');
    /* 空参数 */
    CHECK(zcs_auth_param_get(NULL, "token", out, sizeof(out)) != 0);
    return 0;
}

static int test_split_query(void)
{
    char name[64];
    char params[ZCS_AUTH_PARAMS_MAX];

    zcs_auth_split_query("live/test?token=x&exp=1", name, sizeof(name), params, sizeof(params));
    CHECK(strcmp(name, "live/test") == 0);
    CHECK(strcmp(params, "token=x&exp=1") == 0);

    zcs_auth_split_query("live/test", name, sizeof(name), params, sizeof(params));
    CHECK(strcmp(name, "live/test") == 0);
    CHECK(params[0] == '\0');

    /* 只取参数 */
    zcs_auth_split_query("rtsp://h/live/test?token=y", NULL, 0, params, sizeof(params));
    CHECK(strcmp(params, "token=y") == 0);
    return 0;
}

static int test_disabled_allows(void)
{
    zcs_auth_options opt;
    zcs_auth_request req;
    zcs_auth_decision dec;

    memset(&opt, 0, sizeof(opt));
    opt.enable = 0;
    zcs_auth_init(&opt);
    zcs_auth_set_verifier(test_verify, NULL);
    g_verify_calls = 0;

    memset(&req, 0, sizeof(req));
    req.action = ZCS_AUTH_ACTION_PLAY;
    req.app = "live";
    req.stream = "s";
    /* 未启用时不管 token 都对，且不调用 verifier */
    CHECK(zcs_auth_check(&req, &dec) == 1);
    CHECK(dec.enable_mp4 == 0);
    CHECK(g_verify_calls == 0);
    CHECK(strcmp(zcs_auth_token_param(), "token") == 0);
    return 0;
}

static int test_enabled_without_verifier_denies(void)
{
    zcs_auth_options opt;
    zcs_auth_request req;

    memset(&opt, 0, sizeof(opt));
    opt.enable = 1;
    zcs_auth_init(&opt);
    zcs_auth_set_verifier(NULL, NULL);

    memset(&req, 0, sizeof(req));
    req.action = ZCS_AUTH_ACTION_PUBLISH;
    req.app = "live";
    req.stream = "s";
    /* fail-closed：配了验签却没有实现 */
    CHECK(zcs_auth_check(&req, NULL) == 0);
    return 0;
}

static int test_verifier_decision(void)
{
    zcs_auth_options opt;
    zcs_auth_request req;
    zcs_auth_decision dec;

    memset(&opt, 0, sizeof(opt));
    opt.enable = 1;
    strncpy(opt.token_param, "token", sizeof(opt.token_param) - 1);
    zcs_auth_init(&opt);
    zcs_auth_set_verifier(test_verify, NULL);

    memset(&req, 0, sizeof(req));
    req.action = ZCS_AUTH_ACTION_PUBLISH;
    req.schema = "rtmp";
    req.app = "live";
    req.stream = "s";

    req.params = "token=good";
    CHECK(zcs_auth_check(&req, &dec) == 1);
    CHECK(dec.allow == 1);
    CHECK(dec.enable_mp4 == 1);
    CHECK(g_seen_action == ZCS_AUTH_ACTION_PUBLISH);

    req.params = "token=bad";
    CHECK(zcs_auth_check(&req, &dec) == 0);
    CHECK(dec.allow == 0);
    CHECK(dec.enable_mp4 == 0);

    req.params = NULL;
    CHECK(zcs_auth_check(&req, &dec) == 0);

    /* 自定义参数名生效 */
    memset(&opt, 0, sizeof(opt));
    opt.enable = 1;
    strncpy(opt.token_param, "sign", sizeof(opt.token_param) - 1);
    zcs_auth_init(&opt);
    CHECK(strcmp(zcs_auth_token_param(), "sign") == 0);

    zcs_auth_fini();
    /* fini 之后回到放行 */
    CHECK(zcs_auth_check(&req, &dec) == 1);
    return 0;
}

int main(void)
{
    int r;

    if ((r = test_params()) != 0) {
        return r;
    }
    if ((r = test_split_query()) != 0) {
        return r;
    }
    if ((r = test_disabled_allows()) != 0) {
        return r;
    }
    if ((r = test_enabled_without_verifier_denies()) != 0) {
        return r;
    }
    if ((r = test_verifier_decision()) != 0) {
        return r;
    }
    printf("test_auth_token: ok\n");
    return 0;
}
