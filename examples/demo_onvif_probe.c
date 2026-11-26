/**
 * @file demo_onvif_probe.c
 * @brief ONVIF 客户端自检工具：发现设备 / 读取 profile 与 RTSP 地址。
 *
 * 用法：
 *   demo_onvif_probe [--discover] [--timeout 3000]
 *   demo_onvif_probe --addr http://192.168.1.64/onvif/device_service \
 *                    [--user admin --pass 12345]
 */
#include "zms/device/onvif/onvif.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    printf("usage: %s [--discover] [--timeout ms] [--addr XADDR] [--user U] [--pass P]\n",
           argv0);
}

static void print_device(const zms_onvif_device *d)
{
    printf("device: xaddr=%s\n", d->xaddr);
    if (d->uuid[0]) {
        printf("        uuid=%s\n", d->uuid);
    }
    if (d->manufacturer[0] || d->model[0]) {
        printf("        vendor=%s model=%s firmware=%s serial=%s\n", d->manufacturer, d->model,
               d->firmware, d->serial);
    }
    if (d->media_xaddr[0]) {
        printf("        media=%s\n", d->media_xaddr);
    }
    if (d->ptz_xaddr[0]) {
        printf("        ptz=%s\n", d->ptz_xaddr);
    }
    if (d->scopes[0]) {
        printf("        scopes=%s\n", d->scopes);
    }
}

static int probe(const char *xaddr, const char *user, const char *pass, int timeout_ms,
                 const float *ptz)
{
    zms_onvif_client *c;
    zms_onvif_profile profiles[16];
    size_t count = 0;
    size_t i;

    c = zms_onvif_connect(xaddr, user, pass, timeout_ms);
    if (!c) {
        fprintf(stderr, "connect failed: %s\n", xaddr);
        return 1;
    }
    print_device(zms_onvif_device_info(c));
    if (zms_onvif_get_profiles(c, profiles, sizeof(profiles) / sizeof(profiles[0]), &count) != 0) {
        fprintf(stderr, "GetProfiles failed\n");
        zms_onvif_close(c);
        return 1;
    }
    for (i = 0; i < count; ++i) {
        printf("profile[%zu]: token=%s name=%s encoding=%s %dx%d fps=%d\n", i, profiles[i].token,
               profiles[i].name, profiles[i].encoding, profiles[i].width, profiles[i].height,
               profiles[i].fps);
        printf("            uri=%s\n", profiles[i].uri[0] ? profiles[i].uri : "(none)");
    }
    if (ptz && count > 0) {
        if (zms_onvif_ptz_move(c, profiles[0].token, ptz[0], ptz[1], ptz[2]) == 0) {
            printf("ptz: move ok (%s p=%.2f t=%.2f z=%.2f)\n", profiles[0].token, ptz[0], ptz[1],
                   ptz[2]);
            (void)zms_onvif_ptz_stop(c, profiles[0].token);
        } else {
            fprintf(stderr, "ptz: move failed\n");
        }
    }
    zms_onvif_close(c);
    return count > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    const char *addr = NULL;
    const char *user = NULL;
    const char *pass = NULL;
    int timeout_ms = 3000;
    int discover = 0;
    int i;
    float ptz[3];
    int have_ptz = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--addr") == 0 && i + 1 < argc) {
            addr = argv[++i];
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            user = argv[++i];
        } else if (strcmp(argv[i], "--pass") == 0 && i + 1 < argc) {
            pass = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--ptz") == 0 && i + 1 < argc) {
            if (sscanf(argv[++i], "%f,%f,%f", &ptz[0], &ptz[1], &ptz[2]) == 3) {
                have_ptz = 1;
            } else {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[i], "--discover") == 0) {
            discover = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!addr || discover) {
        zms_onvif_device found[32];
        size_t count = 0;
        size_t k;
        if (zms_onvif_discover(timeout_ms, found, sizeof(found) / sizeof(found[0]), &count) != 0) {
            fprintf(stderr, "discover failed\n");
            return 1;
        }
        printf("discovered %zu device(s)\n", count);
        for (k = 0; k < count; ++k) {
            print_device(&found[k]);
        }
        if (!addr) {
            return count ? 0 : 1;
        }
    }
    return probe(addr, user, pass, timeout_ms, have_ptz ? ptz : NULL);
}
