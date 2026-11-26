/**
 * @file netcheck.h
 * @brief 本机网卡地址自检（校验 externIP 是否是本机地址）。
 */
#ifndef ZMS_OPS_CONFIG_NETCHECK_H
#define ZMS_OPS_CONFIG_NETCHECK_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 判断 @a ip 是否是本机某个网卡上的 IPv4 地址。
 *
 * WebRTC 用 [general] externIP 生成 ICE 候选；填了别的机器的地址会导致
 * 信令成功但媒体不通。启动时用本函数校验并告警。
 *
 * @return 1 = 是本机地址（或无法枚举网卡时保守返回 1）；0 = 明确不是本机地址。
 */
int zms_netcheck_host_ip_present(const char *ip);

#ifdef __cplusplus
}
#endif

#endif /* ZMS_OPS_CONFIG_NETCHECK_H */
