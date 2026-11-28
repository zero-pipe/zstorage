/**
 * @file zcs_test.h
 * @brief 测试断言工具（header-only，零依赖）。
 *
 * 设计目的：让"断言失败"这件事本身就能读，不需要再回去翻测试代码。
 *
 * 反面例子（旧写法，本仓库大量存在）：
 *     CHECK(ts == 90000u);
 *   失败输出：check failed: ts == 90000u
 *   → 读者只知道挂了，不知道实际算出来是多少。是 89999？还是 0？
 *
 * 本头提供带实测值的断言：
 *     CHECK_EQ_U32(ts, 90000u);
 *   失败输出：
 *     FAIL tests/xxx.c:42: zcs_egress_clock_rtp_ts(...) 返回 89999，期望 90000
 *                       （场景：90kHz 时钟，1000ms 应对应 90000 ticks）
 *
 * 用法约定：
 *   - 每个测试用例函数返回 int（0 通过 / 非 0 失败），配 TEST_CASE 报告名字
 *   - 断言失败时打印：文件:行、能力名、实测值、期望值、场景说明
 *   - 一个用例内首条断言失败即返回（不要连续报错淹没输出）
 *
 * 注意：本头用 setjmp/longjmp 实现"断言失败即跳出当前用例"，
 * 因此不要在断言与 longjmp 之间持有需要释放的资源——测试里请用栈对象。
 */
#ifndef ZCS_TESTS_ZCS_TEST_H
#define ZCS_TESTS_ZCS_TEST_H

#include <inttypes.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static jmp_buf g_zcs_test_jmp;
static int g_zcs_test_failed;
static int g_zcs_test_checks;
static int g_zcs_test_failures;
static int g_zcs_test_case_count;
static const char *g_zcs_test_case = "";

/** 当前用例的失败信息（供 main 汇总打印）。 */
static char g_zcs_test_msg[512];

/** 断言失败：记录并跳出当前用例。 */
#define ZCS_TEST_FAIL_(fmt, ...)                                                                   \
    do {                                                                                           \
        snprintf(g_zcs_test_msg, sizeof(g_zcs_test_msg), "%s:%d [%s] " fmt, __FILE__, __LINE__,    \
                 g_zcs_test_case, __VA_ARGS__);                                                    \
        g_zcs_test_failed = 1;                                                                     \
        longjmp(g_zcs_test_jmp, 1);                                                                \
    } while (0)

/** 布尔断言。cond 为假即失败。 */
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_zcs_test_checks;                                                                       \
        if (!(cond)) {                                                                             \
            ZCS_TEST_FAIL_("断言失败: %s", #cond);                                                 \
        }                                                                                          \
    } while (0)

/** 带场景说明的布尔断言。why 写"这个断言在验证什么"。 */
#define CHECK_WHY(cond, why)                                                                       \
    do {                                                                                           \
        ++g_zcs_test_checks;                                                                       \
        if (!(cond)) {                                                                             \
            ZCS_TEST_FAIL_("%s（场景：%s）", #cond, why);                                         \
        }                                                                                          \
    } while (0)

/** 无符号 32 位相等断言，失败时打印实测/期望。 */
#define CHECK_EQ_U32(got, want)                                                                    \
    do {                                                                                           \
        uint32_t _g = (uint32_t)(got);                                                             \
        uint32_t _w = (uint32_t)(want);                                                            \
        ++g_zcs_test_checks;                                                                       \
        if (_g != _w) {                                                                            \
            ZCS_TEST_FAIL_("%s = %" PRIu32 "，期望 %" PRIu32, #got, _g, _w);                       \
        }                                                                                          \
    } while (0)

/** 有符号/通用整数相等断言。 */
#define CHECK_EQ_INT(got, want)                                                                    \
    do {                                                                                           \
        long long _g = (long long)(got);                                                           \
        long long _w = (long long)(want);                                                          \
        ++g_zcs_test_checks;                                                                       \
        if (_g != _w) {                                                                            \
            ZCS_TEST_FAIL_("%s = %lld，期望 %lld", #got, _g, _w);                                  \
        }                                                                                          \
    } while (0)

/** size_t 相等断言。 */
#define CHECK_EQ_SIZE(got, want)                                                                   \
    do {                                                                                           \
        size_t _g = (size_t)(got);                                                                 \
        size_t _w = (size_t)(want);                                                                \
        ++g_zcs_test_checks;                                                                       \
        if (_g != _w) {                                                                            \
            ZCS_TEST_FAIL_("%s = %zu，期望 %zu", #got, _g, _w);                                    \
        }                                                                                          \
    } while (0)

/** 字符串相等断言（NULL 安全），失败时打印两边内容。 */
#define CHECK_STR(got, want)                                                                       \
    do {                                                                                           \
        const char *_g = (got);                                                                    \
        const char *_w = (want);                                                                   \
        ++g_zcs_test_checks;                                                                       \
        if (!_g || !_w || strcmp(_g, _w) != 0) {                                                   \
            ZCS_TEST_FAIL_("%s = \"%s\"，期望 \"%s\"", #got, _g ? _g : "(null)",                    \
                           _w ? _w : "(null)");                                                    \
        }                                                                                          \
    } while (0)

/**
 * 声明一个测试用例：自动打印用例名，失败时记录并进入下一用例。
 * 用法：
 *     static void test_xxx(void) { ... }
 *     ...
 *     RUN_TEST(test_xxx);
 */
#define RUN_TEST(fn)                                                                               \
    do {                                                                                           \
        g_zcs_test_case = #fn;                                                                     \
        g_zcs_test_failed = 0;                                                                     \
        if (setjmp(g_zcs_test_jmp) == 0) {                                                         \
            fn();                                                                                  \
        }                                                                                          \
        if (g_zcs_test_failed) {                                                                   \
            fprintf(stderr, "FAIL %s\n", g_zcs_test_msg);                                          \
            ++g_zcs_test_failures;                                                                 \
        } else {                                                                                   \
            printf("  ok   %s\n", #fn);                                                            \
        }                                                                                          \
    } while (0)

/** main 收尾：打印汇总并返回进程退出码（0=全过）。 */
#define ZCS_TEST_SUMMARY(name) \
    do { \
        if (g_zcs_test_failures) { \
            fprintf(stderr, "\n%s: %d case(s) FAILED (%d checks)\n", name, \
                    g_zcs_test_failures, g_zcs_test_checks); \
            return 1; \
        } \
        printf("\n%s: OK (%d cases, %d checks)\n", name, g_zcs_test_case_count, \
               g_zcs_test_checks); \
        return 0; \
    } while (0)

/** 用例计数：RUN_TEST 前 +1，便于汇总输出。 */
#define RUN_TEST_COUNTED(fn)                                                                       \
    do {                                                                                           \
        ++g_zcs_test_case_count;                                                                   \
        RUN_TEST(fn);                                                                              \
    } while (0)

#endif /* ZCS_TESTS_ZCS_TEST_H */
