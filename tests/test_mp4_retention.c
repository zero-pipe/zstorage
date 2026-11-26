/**
 * @file test_mp4_retention.c
 * @brief 录制磁盘治理：总量上限时按最旧删除、未超限不动、无上限不动。
 *        直接构造目录与文件，不依赖真实录制器。
 */
#include "zms/egress/recorder/egress_mp4_recorder.h"
#include "zms/ops/config/config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define TEST_ROOT "./test_mp4_ret"

static void write_file(const char *path, size_t bytes, const char *content)
{
    FILE *fp = fopen(path, "wb");
    size_t i;

    if (!fp) {
        return;
    }
    for (i = 0; i < bytes; ++i) {
        fputc(content[i % strlen(content)], fp);
    }
    fclose(fp);
}

#if !defined(_WIN32)
static void wipe_tree(void)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", TEST_ROOT);
    (void)system(cmd);
}
#else
static void wipe_tree(void) {}
#endif

/* 建两路流、每路一个日期目录，各放 3 个 1MB 文件，共 6MB。 */
static int setup_tree(void)
{
#if defined(_WIN32)
    return -1; /* 用例仅 POSIX */
#else
    wipe_tree();
    mkdir(TEST_ROOT, 0755);
    mkdir(TEST_ROOT "/cam1", 0755);
    mkdir(TEST_ROOT "/cam1/2026-11-13", 0755);
    mkdir(TEST_ROOT "/cam2", 0755);
    mkdir(TEST_ROOT "/cam2/2026-11-13", 0755);
    for (int i = 0; i < 3; ++i) {
        char p[256];
        snprintf(p, sizeof(p), TEST_ROOT "/cam1/2026-11-13/a%d.mp4", i);
        write_file(p, 1024 * 1024, "A");
        snprintf(p, sizeof(p), TEST_ROOT "/cam2/2026-11-13/b%d.mp4", i);
        write_file(p, 1024 * 1024, "B");
    }
    return 0;
#endif
}

int main(void)
{
    zms_record_config rec;

    if (setup_tree() != 0) {
        printf("test_mp4_retention: skipped (non-POSIX)\n");
        return 0;
    }

    /* 1) 无上限：不删任何文件。 */
    memset(&rec, 0, sizeof(rec));
    snprintf(rec.root, sizeof(rec.root), "%s", TEST_ROOT);
    rec.max_total_mb = 0;
    zms_mp4_recorder_configure(&rec);
    CHECK(zms_mp4_recorder_enforce_retention() == 0);

    /* 2) 上限 3MB：总量 6MB，应删到 <=3MB（删掉至少一半文件）。 */
    rec.max_total_mb = 3;
    zms_mp4_recorder_configure(&rec);
    {
        int removed = zms_mp4_recorder_enforce_retention();

        CHECK(removed >= 3); /* 需删至少 3 个 1MB 文件才能到 3MB 以内 */
    }

    /* 3) 再跑一次：已经在上限内，不应再删。 */
    CHECK(zms_mp4_recorder_enforce_retention() == 0);

    wipe_tree();
    printf("test_mp4_retention: ok\n");
    return 0;
}
