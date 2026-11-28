/**
 * @file auth.h
 * @brief 推流/播放鉴权：上游签发 token，流媒体只做本地验签。
 *
 * 边界（见 docs/stream-pipeline.md 第 3.2 节）：
 *   - 推流 app 与播放 app 先到 web 后台完成业务鉴权，后台签发带时效的 token，
 *     客户端把 token 放在协议 URL 的查询参数里发起推/拉流；
 *   - 流媒体进程只做**本地验签**：不解码业务身份、不查库、不回调 webhook；
 *   - 验签必须是纯本地计算，poller 线程可直接同步调用，绝不阻塞、绝不发网络请求。
 *
 * 本轮只落骨架：验签实现由上层通过 zcs_auth_set_verifier() 注册；
 * 未启用（[auth] enable=0，默认）时一律放行；启用但未注册实现时 fail-closed。
 */
#ifndef ZCS_CORE_AUTH_H
#define ZCS_CORE_AUTH_H

#include "zcs/zcs_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 上游参数串（URL query，不含 '?'）的最大长度；超出部分截断。 */
#define ZCS_AUTH_PARAMS_MAX 512
/** token 参数名最大长度。 */
#define ZCS_AUTH_PARAM_NAME_MAX 32

/** 鉴权动作 */
typedef enum zcs_auth_action {
    ZCS_AUTH_ACTION_PUBLISH = 0, /**< 外部主动推流 */
    ZCS_AUTH_ACTION_PLAY = 1,    /**< 播放 */
} zcs_auth_action;

/**
 * 一次鉴权请求。字段全部为 borrowed 指针，只在调用期间有效；
 * 未取到的字段为 NULL。
 */
typedef struct zcs_auth_request {
    zcs_auth_action action;
    /** 接入协议：rtmp / rtsp / srt / rtp-ps / webrtc / http-flv / http-ts / hls / http-mp4 */
    const char *schema;
    const char *vhost;
    const char *app;
    const char *stream;
    /** 客户端原始查询串（不含 '?'），如 "token=xxx&exp=1700000000"；可为 NULL */
    const char *params;
    /** 对端 IPv4/IPv6 文本；可为 NULL */
    const char *ip;
    unsigned port;
    /** 协议自带的会话标识（SRT streamid、HTTP id 等）；可为 NULL */
    const char *id;
} zcs_auth_request;

/** 鉴权结论 */
typedef struct zcs_auth_decision {
    int allow;      /**< 1=允许，0=拒绝 */
    int enable_mp4; /**< 仅推流有效：1=启动直播 MP4 录制（由 token 声明决定） */
} zcs_auth_decision;

/**
 * 验签实现（预留挂点）。由上层（web 后台对接方）注册。
 *
 * 约束：在 poller 线程被调用，必须快速返回，不得阻塞或发网络请求。
 * @return 0=已完成，结论写入 dec；非 0=无法判定，按拒绝处理。
 */
typedef int (*zcs_auth_verify_fn)(const zcs_auth_request *req, zcs_auth_decision *dec,
                                  void *user);

/** 初始化选项（由 ops 层从 [auth] 配置填充）。 */
typedef struct zcs_auth_options {
    /** 0=不验签（全放行）；1=验签 */
    int enable;
    /** token 参数名，默认 "token" */
    char token_param[ZCS_AUTH_PARAM_NAME_MAX];
} zcs_auth_options;

/** 初始化；opt 为 NULL 时取默认（不启用）。进程启动调用一次。 */
ZCS_API void zcs_auth_init(const zcs_auth_options *opt);
/** 关闭清理；之后调用 zcs_auth_check 一律放行。 */
ZCS_API void zcs_auth_fini(void);

/** 注册验签实现；传 NULL 注销。 */
ZCS_API void zcs_auth_set_verifier(zcs_auth_verify_fn fn, void *user);

/**
 * 设置接入 token 白名单（CSV，逗号分隔）。由平台通过 /index/api/auth/tokens 同步。
 *
 * 非空：运行期启用推流本地验签——未注册自定义验签实现时用内置的“token 是否在白名单”
 *       判断；置空：清除白名单并关闭验签（恢复全放行）。
 * 线程说明：由 WebAPI 处理线程调用，内部整串覆盖写，读取端每次比对时现场切分。
 */
ZCS_API void zcs_auth_set_tokens(const char *csv);

/**
 * 鉴权入口：同步、无锁、非阻塞，可在任意 poller 线程调用。
 * @param req 请求（必填）
 * @param dec 结论输出；可为 NULL
 * @return 1=允许，0=拒绝
 */
ZCS_API int zcs_auth_check(const zcs_auth_request *req, zcs_auth_decision *dec);

/** 当前 token 参数名（默认 "token"）。 */
ZCS_API const char *zcs_auth_token_param(void);

/**
 * 从查询串中取参数值（URL 解码到 out，遇到 '&' 或结尾为止）。
 * @return 0=找到；-1=未找到或参数过长
 */
ZCS_API int zcs_auth_param_get(const char *params, const char *name, char *out, size_t cap);

/** 把查询串 copy 到定长缓冲（截断，不做解码）；没有时写入空串。 */
ZCS_API void zcs_auth_params_copy(const char *params, char *out, size_t cap);

/**
 * 把 "name?query" 拆成名字与查询串（原地不改入参）。
 * 没有 '?' 时 name 为原串、params 为空串。
 */
ZCS_API void zcs_auth_split_query(const char *in, char *name, size_t name_cap, char *params,
                                  size_t params_cap);

struct zn_tcp_session;
typedef struct zn_tcp_session zn_tcp_session;
/**
 * 取 TCP 会话的对端 IP/端口，填入鉴权请求。
 * 无会话或取不到时 ip 写空串、port 写 0。
 */
ZCS_API void zcs_auth_fill_peer(zn_tcp_session *tcp, char *ip, size_t cap, unsigned *port);

#ifdef __cplusplus
}
#endif

#endif /* ZCS_CORE_AUTH_H */
