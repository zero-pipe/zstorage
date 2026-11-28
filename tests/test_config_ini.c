/**
 * @file test_config_ini.c
 * @brief 配置模板与代码表一致性：conf/config.ini 不得有未知键，且不能漏掉受支持的键。
 *
 * 目的：config.ini 是用户唯一可见的配置文档。历史上出现过
 *   - 模板里写了代码不认识的键（静默失效，用户以为生效）
 *   - 代码新增了键却没写进模板（用户不知道有这个能力）
 * 本测试把两边钉死，防止再次漂移。
 */
#include "zcs/ops/config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ZCS_CONFIG_INI
#error "ZCS_CONFIG_INI must point at conf/config.ini"
#endif

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define MAX_KEYS 512

typedef struct {
    char section[MAX_KEYS][32];
    char key[MAX_KEYS][48];
    char alias[MAX_KEYS][48];
    int count;
} key_list;

static void collect_supported(const char *section, const char *key, const char *alias, void *user)
{
    key_list *l = (key_list *)user;

    if (!l || l->count >= MAX_KEYS) {
        return;
    }
    snprintf(l->section[l->count], sizeof(l->section[0]), "%s", section);
    snprintf(l->key[l->count], sizeof(l->key[0]), "%s", key);
    snprintf(l->alias[l->count], sizeof(l->alias[0]), "%s", alias ? alias : "");
    ++l->count;
}

static int list_contains(const key_list *l, const char *section, const char *key)
{
    int i;

    for (i = 0; i < l->count; ++i) {
        if (strcmp(l->section[i], section) == 0 && strcmp(l->key[i], key) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 模板里出现的键（按 section 记录；仅未注释的行算真实配置） */
static key_list g_template;
/* 模板里以注释形式记录文档的键（如废弃/别名项） */
static key_list g_documented;

static int scan_template(const char *path)
{
    FILE *fp;
    char line[1024];
    char section[32] = "general";

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", path);
        return -1;
    }
    while (fgets(line, sizeof(line), fp)) {
        char *s = line;
        char *eq;

        /* trim 左右空白 */
        while (*s == ' ' || *s == '\t') {
            ++s;
        }
        {
            size_t n = strlen(s);
            while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ')) {
                s[--n] = '\0';
            }
        }
        if (!s[0]) {
            continue;
        }
        if (s[0] == '#' || s[0] == ';') {
            /* 注释行里若形如 "key=..."，视为「已文档化的兼容/别名项」 */
            char *k = s + 1;
            char *ceq;

            while (*k == ' ' || *k == '#') {
                ++k;
            }
            ceq = strchr(k, '=');
            if (ceq) {
                *ceq = '\0';
                {
                    size_t n = strlen(k);
                    while (n > 0 && (k[n - 1] == ' ')) {
                        k[--n] = '\0';
                    }
                }
                if (k[0] && g_documented.count < MAX_KEYS) {
                    snprintf(g_documented.section[g_documented.count],
                             sizeof(g_documented.section[0]), "%s", section);
                    snprintf(g_documented.key[g_documented.count], sizeof(g_documented.key[0]),
                             "%s", k);
                    ++g_documented.count;
                }
            }
            continue;
        }
        if (s[0] == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", s + 1);
            }
            continue;
        }
        eq = strchr(s, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        {
            size_t n = strlen(s);
            while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
                s[--n] = '\0';
            }
        }
        if (g_template.count < MAX_KEYS) {
            snprintf(g_template.section[g_template.count], sizeof(g_template.section[0]), "%s",
                     section);
            snprintf(g_template.key[g_template.count], sizeof(g_template.key[0]), "%s", s);
            ++g_template.count;
        }
    }
    fclose(fp);
    return 0;
}

int main(void)
{
    key_list supported;
    int i;
    int bad = 0;

    memset(&supported, 0, sizeof(supported));
    zcs_config_foreach_key(collect_supported, &supported);
    CHECK(supported.count > 0);

    memset(&g_template, 0, sizeof(g_template));
    memset(&g_documented, 0, sizeof(g_documented));
    CHECK(scan_template(ZCS_CONFIG_INI) == 0);
    CHECK(g_template.count > 0);

    /* 1) 模板里的每个键都必须是受支持的（拼写错误在这里暴露） */
    for (i = 0; i < g_template.count; ++i) {
        if (!zcs_config_key_supported(g_template.section[i], g_template.key[i])) {
            fprintf(stderr, "FAIL config.ini has unsupported key: [%s] %s\n",
                    g_template.section[i], g_template.key[i]);
            ++bad;
        }
    }

    /* 2) 代码支持的每个键都必须出现在模板里（主键或别名均可，新能力不会被遗忘） */
    for (i = 0; i < supported.count; ++i) {
        if (!list_contains(&g_template, supported.section[i], supported.key[i]) &&
            !(supported.alias[i][0] &&
              list_contains(&g_template, supported.section[i], supported.alias[i])) &&
            !list_contains(&g_documented, supported.section[i], supported.key[i])) {
            fprintf(stderr, "FAIL config.ini missing supported key: [%s] %s\n",
                    supported.section[i], supported.key[i]);
            ++bad;
        }
    }

    if (bad) {
        fprintf(stderr, "test_config_ini: %d mismatch(es)\n", bad);
        return 1;
    }
    printf("test_config_ini: OK (%d keys supported, %d in template)\n", supported.count,
           g_template.count);
    return 0;
}
