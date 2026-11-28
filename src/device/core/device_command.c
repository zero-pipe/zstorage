/**
 * @file device_command.c
 * @brief 设备命令表查找与能力门控。
 */
#include "zcs/device/core/device_command.h"

#include <string.h>

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

const zcs_device_cmd *zcs_device_cmd_find(const zcs_device_cmd *table, size_t count,
                                          const char *name)
{
    size_t i;

    if (!table || !name) {
        return NULL;
    }
    for (i = 0; i < count; ++i) {
        if (table[i].name && ieq(table[i].name, name)) {
            return &table[i];
        }
    }
    return NULL;
}

int zcs_device_cmd_allowed(const zcs_device_cmd *cmd)
{
    if (!cmd) {
        return 0;
    }
    if (cmd->capability == 0) {
        return 1;
    }
    return zcs_device_core_any_cap(cmd->capability);
}
