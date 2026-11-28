/**
 * GB28181 设备端演示 / 本地联调进程：
 *   RTMP 收流 + HTTP API + GB28181 注册与 PS/RTP 推流。
 *
 *   demo_gb28181_device [--config config.ini] [--log file] [--seconds N]
 */
#include "demo_server_runtime.h"
#include "zcs/device/gb28181/gb28181.h"
#include "zcs/device/gb28181/gb28181_platform.h"
#include "zcs/egress/recorder/egress_mp4_recorder.h"
#include "zcs/session/http/http_server.h"
#include "zcs/session/rtmp/rtmp_server.h"
#include "zn/util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zcs_gb28181_opts gb_opts_from_cfg(const zcs_config *cfg)
{
    zcs_gb28181_opts o;

    memset(&o, 0, sizeof(o));
    o.platform_id = cfg->gb28181.platform_id;
    o.platform_domain = cfg->gb28181.platform_domain;
    o.platform_ip = cfg->gb28181.platform_ip;
    o.platform_port = cfg->gb28181.platform_port;
    o.device_id = cfg->gb28181.device_id;
    o.device_name = cfg->gb28181.device_name;
    o.manufacturer = cfg->gb28181.manufacturer;
    o.model = cfg->gb28181.model;
    o.firmware = cfg->gb28181.firmware;
    o.username = cfg->gb28181.username;
    o.password = cfg->gb28181.password;
    o.local_ip = cfg->gb28181.local_ip;
    o.local_port = cfg->gb28181.local_port;
    o.sip_transport = cfg->gb28181.sip_transport;
    o.sip_mode = cfg->gb28181.sip_mode;
    o.tls_ca = cfg->gb28181.tls_ca;
    o.tls_cert = cfg->gb28181.tls_cert;
    o.tls_key = cfg->gb28181.tls_key;
    o.tls_verify_peer = cfg->gb28181.tls_verify_peer;
    o.register_expires = cfg->gb28181.register_expires;
    o.keepalive_interval = cfg->gb28181.keepalive_interval;
    o.channels = cfg->gb28181.channels;
    o.record_root = cfg->gb28181.record_root;
    return o;
}

int main(int argc, char **argv)
{
    zcs_demo_server_args args;
    zcs_demo_server_runtime rt;
    zcs_http_server *http = NULL;
    zcs_rtmp_server *rtmp = NULL;
    zcs_gb28181_opts gopts;

    zcs_demo_server_args_default(&args);
    if (zcs_demo_server_parse_args(argc, argv, &args) != 0) {
        zcs_demo_server_print_usage(argv[0], "");
        return 1;
    }
    if (zcs_demo_server_runtime_init(&rt, &args) != ZN_OK) {
        return 1;
    }
    zcs_mp4_recorder_configure(&rt.cfg.record);

    {
        zcs_http_server_opts hopts;
        memset(&hopts, 0, sizeof(hopts));
        hopts.poller_pool = rt.poller_pool;
        hopts.host = "0.0.0.0";
        hopts.port = (uint16_t)rt.cfg.http.port;
        hopts.api_secret = rt.cfg.http.api_secret;
        hopts.cfg = &rt.cfg;
        http = zcs_http_server_create(&hopts);
        if (!http || zcs_http_server_start(http) != ZN_OK) {
            zn_error("HTTP service start failed");
            goto done;
        }
        printf("HTTP API listen %u\r\n", (unsigned)zcs_http_server_port(http));
    }

    {
        zcs_rtmp_server_opts ropts;
        memset(&ropts, 0, sizeof(ropts));
        ropts.poller_pool = rt.poller_pool;
        ropts.host = "0.0.0.0";
        ropts.port = (uint16_t)rt.cfg.rtmp.port;
        rtmp = zcs_rtmp_server_create(&ropts);
        if (!rtmp || zcs_rtmp_server_start(rtmp) != ZN_OK) {
            zn_error("RTMP service start failed");
            goto done;
        }
        printf("RTMP listen %u\r\n", (unsigned)zcs_rtmp_server_port(rtmp));
    }

    if (rt.cfg.gb28181_platform.enable) {
        zcs_gb28181_platform_opts popts;
        memset(&popts, 0, sizeof(popts));
        popts.id = rt.cfg.gb28181_platform.id;
        popts.domain = rt.cfg.gb28181_platform.domain;
        popts.password = rt.cfg.gb28181_platform.password;
        popts.advertise_ip = rt.cfg.gb28181_platform.advertise_ip;
        popts.listen_port = rt.cfg.gb28181_platform.listen_port;
        popts.transport = rt.cfg.gb28181_platform.transport;
        popts.tls_cert = rt.cfg.gb28181_platform.tls_cert;
        popts.tls_key = rt.cfg.gb28181_platform.tls_key;
        popts.tls_ca = rt.cfg.gb28181_platform.tls_ca;
        popts.tls_verify_peer = rt.cfg.gb28181_platform.tls_verify_peer;
        popts.keepalive_timeout_sec = rt.cfg.gb28181_platform.keepalive_timeout_sec;
        popts.media_vhost = rt.cfg.gb28181_platform.media_vhost;
        popts.media_app = rt.cfg.gb28181_platform.media_app;
        if (zcs_gb28181_platform_start(&popts) != 0) {
            zn_warn("GB28181 platform start failed");
        }
    }

    if (rt.cfg.gb28181.enable) {
        gopts = gb_opts_from_cfg(&rt.cfg);
        gopts.poller_pool = rt.poller_pool;
        if (zcs_gb28181_start(&gopts) != 0) {
            zn_warn("GB28181 device start failed");
        }
    }

    zcs_demo_server_runtime_wait(&rt, args.seconds);

done:
    zcs_gb28181_stop();
    zcs_gb28181_platform_stop();
    if (rtmp) {
        zcs_rtmp_server_stop(rtmp);
        zcs_rtmp_server_destroy(rtmp);
    }
    if (http) {
        zcs_http_server_stop(http);
        zcs_http_server_destroy(http);
    }
    zcs_demo_server_runtime_fini(&rt);
    return 0;
}
