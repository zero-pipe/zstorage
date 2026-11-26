/**
 * @file zm_file_io.c
 * @brief 文件型 `zm_io` 适配器：把 FILE* 包成 ZM reader 需要的 read/seek/tell。
 */
#include "bridge/zm_file_io.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
#define ZMS_FSEEK64 _fseeki64
#define ZMS_FTELL64 _ftelli64
#elif defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#define ZMS_FSEEK64 fseeko
#define ZMS_FTELL64 ftello
#else
#define ZMS_FSEEK64 fseek
#define ZMS_FTELL64 ftell
#endif

typedef struct {
    FILE *fp;
} zms_zm_file_ctx;

/* read: >0 字节数，0 EOF，<0 错误（zm_io 契约）。 */
static int zms_fio_read(void *ctx, void *buf, size_t len)
{
    zms_zm_file_ctx *c = (zms_zm_file_ctx *)ctx;
    size_t n;

    if (!c || !c->fp || (!buf && len)) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }
    n = fread(buf, 1, len, c->fp);
    if (n > 0) {
        return (int)n;
    }
    return ferror(c->fp) ? -1 : 0;
}

static int zms_fio_seek(void *ctx, int64_t off, int whence)
{
    zms_zm_file_ctx *c = (zms_zm_file_ctx *)ctx;

    if (!c || !c->fp) {
        return -1;
    }
    return ZMS_FSEEK64(c->fp, (long)off, whence) == 0 ? 0 : -1;
}

static int64_t zms_fio_tell(void *ctx)
{
    zms_zm_file_ctx *c = (zms_zm_file_ctx *)ctx;

    if (!c || !c->fp) {
        return -1;
    }
    return (int64_t)ZMS_FTELL64(c->fp);
}

static void zms_fio_close(void *ctx)
{
    zms_zm_file_ctx *c = (zms_zm_file_ctx *)ctx;

    if (!c) {
        return;
    }
    if (c->fp) {
        fclose(c->fp);
        c->fp = NULL;
    }
    free(c);
}

int zms_zm_file_io_open(const char *path, zm_io *io)
{
    zms_zm_file_ctx *c;

    if (!path || !path[0] || !io) {
        return -1;
    }
    c = (zms_zm_file_ctx *)calloc(1, sizeof(*c));
    if (!c) {
        return -1;
    }
    c->fp = fopen(path, "rb");
    if (!c->fp) {
        free(c);
        return -1;
    }
    io->ctx = c;
    io->read = zms_fio_read;
    io->seek = zms_fio_seek;
    io->tell = zms_fio_tell;
    io->close = zms_fio_close;
    return 0;
}

void zms_zm_file_io_close(zm_io *io)
{
    if (!io) {
        return;
    }
    if (io->close) {
        io->close(io->ctx);
    }
    io->ctx = NULL;
    io->read = NULL;
    io->seek = NULL;
    io->tell = NULL;
    io->close = NULL;
}
