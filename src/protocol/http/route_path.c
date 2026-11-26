/**
 * @file route_path.c
 * @brief HTTP 路由路径解析（FLV/TS/HLS 路径 → app/stream/文件）。
 */
#include "zms/play/vod_play_port.h"
#include "zms/protocol/http/request_reader.h"
#include "zms/protocol/http/session_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <sys/stat.h>
#else
#include <strings.h>
#include <sys/stat.h>
#endif

static int parse_live_hls_ts_name(const char *name, char *stream, char *file, size_t file_cap);
static int parse_live_hls_ts_slash_name(const char *name, char *stream, char *file,
                                        size_t file_cap);

/* 从 query 里取整数参数（秒）。strstr 找 "key=" 而非严格解析，容忍前面有别的
 * 参数；找不到返回 0，让上层继续尝试下一个别名。 */
static int parse_query_uint64(const char *query, const char *key, uint64_t *out)
{
    char token[32];
    const char *p;

    snprintf(token, sizeof(token), "%s=", key);
    p = strstr(query, token);
    if (!p) {
        return 0;
    }
    *out = (uint64_t)strtoull(p + strlen(token), NULL, 10);
    return 1;
}

/* 同 parse_query_uint64，但按“秒浮点”解析并 +0.5 四舍五入到毫秒——seek 参数
 * 常带小数秒（如 t=12.34），直接转整数会丢掉亚秒精度导致 seek 落点偏移。 */
static int parse_query_double_ms(const char *query, const char *key, uint64_t *out)
{
    char token[32];
    const char *p;

    snprintf(token, sizeof(token), "%s=", key);
    p = strstr(query, token);
    if (!p) {
        return 0;
    }
    *out = (uint64_t)(strtod(p + strlen(token), NULL) * 1000.0 + 0.5);
    return 1;
}

/*
 * 解析 FLV 请求里的 seek 起点。各家播放器用的参数名不统一（ffplay、flv.js、
 * VLC、EasyPlayer 各有偏好），故依次尝试一串别名；第一个命中即返回，避免
 * 后被覆盖。start/offset/pos 是秒（整数），其余按秒浮点再转毫秒（四舍五入）。
 * 末尾多出的 pos 调用是历史兼容：有的客户端 pos 用浮点秒而非整数。
 */
static void parse_flv_query(const char *query, uint64_t *seek_ms)
{
    if (!query || !seek_ms) {
        return;
    }
    if (*query == '?') {
        query++;
    }
    if (parse_query_uint64(query, "start", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "seek", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "t", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "time", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "fs", seek_ms)) {
        return;
    }
    if (parse_query_double_ms(query, "ec_seek", seek_ms)) {
        return;
    }
    if (parse_query_uint64(query, "offset", seek_ms)) {
        return;
    }
    if (parse_query_uint64(query, "pos", seek_ms)) {
        return;
    }
    parse_query_double_ms(query, "pos", seek_ms);
}

static int path_ends_with_ci(const char *path, const char *suffix)
{
    size_t plen;
    size_t slen;

    if (!path || !suffix) {
        return 0;
    }
    plen = strlen(path);
    slen = strlen(suffix);
    if (plen < slen) {
        return 0;
    }
#ifdef _WIN32
    return _stricmp(path + plen - slen, suffix) == 0;
#else
    return strcasecmp(path + plen - slen, suffix) == 0;
#endif
}

/*
 * 解析请求的 vhost：URL 查询参数 ?vhost= 优先（显式指定最可靠），
 * 否则用 Host 头归一化得到。多域名部署时 vhost 决定去哪找流，二者都不能拿来
 * 时归一化会给出默认 vhost。
 */
void zms_http_request_vhost(const zms_http_request *req, char *out, size_t cap)
{
    const char *q;
    const char *v;

    if (!out || cap == 0) {
        return;
    }
    out[0] = 0;
    if (req && req->path[0]) {
        q = strchr(req->path, '?');
        if (q) {
            v = strstr(q + 1, "vhost=");
            if (v) {
                size_t n = 0;
                v += 6;
                while (v[n] && v[n] != '&' && n + 1 < cap) {
                    out[n] = v[n];
                    ++n;
                }
                out[n] = 0;
                if (n > 0) {
                    char norm[ZMS_VHOST_MAX];
                    zms_vhost_normalize(out, norm, sizeof(norm));
                    strncpy(out, norm, cap - 1);
                    out[cap - 1] = 0;
                    return;
                }
            }
        }
    }
    zms_vhost_normalize(req ? zms_http_req_header(req, "Host") : NULL, out, cap);
}

/*
 * 解析 HTTP-FLV 请求路径为 app/stream/seek。
 *
 * 路由分派有明显的“record app 隔离”设计：
 *   - 当 app 是点播 app（默认 vod）时，只认两种磁盘点播形态（.flv 包装后缀 /
 *     原生 .flv）；其他一切形态（如 {app}/{stream}.flv 直播）都直接返回空，
 *     避免点播 app 下误把普通路径当直播流；
 *   - 非点播 app 才走直播的 .flv / .live.flv 兼容解析。
 * 这样同一套路径规则不会在点播与直播之间串味。
 */
void zms_http_route_parse_flv_path(const char *path, char *app, char *stream, uint64_t *seek_ms)
{
    char path_only[512];
    const char *q;
    size_t plen;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (seek_ms) {
        *seek_ms = 0;
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return;
    }
    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';
    if (q) {
        parse_flv_query(q, seek_ms);
    }

    const char *p = path_only + 1;
    const char *slash = strchr(p, '/');
    if (!slash) {
        strncpy(app, p, ZMS_APP_MAX - 1);
        return;
    }
    size_t alen = (size_t)(slash - p);
    if (alen >= ZMS_APP_MAX) {
        alen = ZMS_APP_MAX - 1;
    }
    memcpy(app, p, alen);
    app[alen] = '\0';
    const char *name = slash + 1;

    /* 点播 HTTP-FLV 封装：{app}/path/to/{file}.{mp4|mkv|mov|m4v}.flv */
    if (zms_vod_is_record_app(app) && zms_vod_stream_is_vod_flv_wrap_suffix(name)) {
        size_t slen = strlen(name) - 4;

        if (slen >= ZMS_STREAM_MAX) {
            slen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        return;
    }

    /* 点播磁盘原生 FLV：{vod_app}/path/to/{file}.flv */
    if (zms_vod_is_record_app(app) && zms_vod_stream_is_native_flv_file(name)) {
        size_t nlen = strlen(name);

        if (nlen >= ZMS_STREAM_MAX) {
            nlen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, nlen);
        stream[nlen] = '\0';
        return;
    }

    if (zms_vod_is_record_app(app)) {
        return;
    }

    /* 直播：{app}/{stream}.flv（stream 可含 /）；ZLM 兼容 {stream}.live.flv */
    if (path_ends_with_ci(name, ".live.flv")) {
        size_t nlen = strlen(name);
        size_t slen = nlen - 9;
        if (slen >= ZMS_STREAM_MAX) {
            slen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        return;
    }
    if (path_ends_with_ci(name, ".flv")) {
        size_t nlen = strlen(name);
        size_t slen = nlen - 4;
        if (slen >= ZMS_STREAM_MAX) {
            slen = ZMS_STREAM_MAX - 1;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        return;
    }
}

int zms_http_route_parse_live_ts_path(const char *path, char *app, char *stream)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }
    if (zms_vod_is_record_app(app)) {
        return 0;
    }

    name = slash + 1;
    if (!name[0] || !path_ends_with_ci(name, ".ts")) {
        return 0;
    }
    {
        char hls_stream[ZMS_STREAM_MAX];
        char hls_file[128];

        if (parse_live_hls_ts_name(name, hls_stream, hls_file, sizeof(hls_file))) {
            return 0;
        }
        if (parse_live_hls_ts_slash_name(name, hls_stream, hls_file, sizeof(hls_file))) {
            return 0;
        }
    }
    {
        size_t slen = strlen(name) - 3;
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
    }
    return 1;
}

int zms_http_route_parse_vod_hls_path(const char *path, char *app, char *stream, char *rel,
                                      size_t rel_cap)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *rel_part;
    size_t alen;
    size_t rlen;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (rel_cap && rel) {
        rel[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream || !rel || rel_cap == 0) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    alen = (size_t)(slash - p);
    if (alen >= ZMS_APP_MAX) {
        alen = ZMS_APP_MAX - 1;
    }
    memcpy(app, p, alen);
    app[alen] = '\0';
    if (!zms_vod_is_record_app(app)) {
        return 0;
    }

    rel_part = slash + 1;
    if (!rel_part[0] || !zms_vod_rel_path_safe(rel_part)) {
        return 0;
    }
    rlen = strlen(rel_part);
    if (rlen >= rel_cap) {
        rlen = rel_cap - 1;
    }
    memcpy(rel, rel_part, rlen);
    rel[rlen] = '\0';

    if (path_ends_with_ci(rel, ".m3u8")) {
        if (!zms_vod_m3u8_rel_to_mp4_stream(rel, stream, ZMS_STREAM_MAX)) {
            return 0;
        }
        return 1;
    }
    if (path_ends_with_ci(rel, ".ts")) {
        return 1;
    }
    return 0;
}

/** m3u8 分片行为纯数字索引文件名 */
/* 判定文件名是否为“纯数字索引分片”：{数字}.{ext}（如 12.ts / 007.m4s）。
 * 直播 HLS 的分片常见这种命名；rewrite 与解析都靠它区分“分片行”与“普通文本行”。
 * 要求扩展名前全是数字，任何其他字符都不算——避免把脚本/静态资源误当分片。 */
static int hls_seg_file_is_index_media(const char *fname, const char *ext, size_t ext_len)
{
    size_t nlen;
    size_t i;

    if (!fname || !ext || ext_len == 0 || !path_ends_with_ci(fname, ext)) {
        return 0;
    }
    nlen = strlen(fname);
    if (nlen < ext_len + 1) {
        return 0;
    }
    for (i = 0; i + ext_len < nlen; ++i) {
        if (fname[i] < '0' || fname[i] > '9') {
            return 0;
        }
    }
    return fname[i] == '.' &&
#if defined(_WIN32)
           _strnicmp(fname + i + 1, ext + 1, ext_len - 1) == 0;
#else
           strncasecmp(fname + i + 1, ext + 1, ext_len - 1) == 0;
#endif
}

static int hls_seg_file_is_index_ts(const char *fname)
{
    return hls_seg_file_is_index_media(fname, ".ts", 3);
}

static int hls_seg_file_is_index_m4s(const char *fname)
{
    return hls_seg_file_is_index_media(fname, ".m4s", 4);
}

/** 旧式目录 TS {app}/{stream}/{N}.ts；以 m3u8 相对路径解析成的 /{app}/{prefix}/{N}.ts */
static int parse_live_hls_ts_slash_name(const char *name, char *stream, char *file, size_t file_cap)
{
    const char *slash;
    const char *fname;
    size_t slen;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    slash = strrchr(name, '/');
    if (!slash || slash == name) {
        return 0;
    }
    fname = slash + 1;
    if (!hls_seg_file_is_index_ts(fname)) {
        return 0;
    }
    slen = (size_t)(slash - name);
    if (slen == 0 || slen >= ZMS_STREAM_MAX) {
        return 0;
    }
    memcpy(stream, name, slen);
    stream[slen] = '\0';
    strncpy(file, fname, file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

/** 直播 fMP4 init {app}/{stream}.init.mp4 */
static int parse_live_hls_init_name(const char *name, char *stream, char *file, size_t file_cap)
{
    const char *suffix = ".init.mp4";
    size_t nlen;
    size_t slen;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    if (!path_ends_with_ci(name, suffix)) {
        return 0;
    }
    nlen = strlen(name);
    slen = nlen - strlen(suffix);
    if (slen == 0 || slen >= ZMS_STREAM_MAX) {
        return 0;
    }
    memcpy(stream, name, slen);
    stream[slen] = '\0';
    strncpy(file, "init.mp4", file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

/** 直播 fMP4 {app}/{stream}.{N}.m4s */
static int parse_live_hls_m4s_name(const char *name, char *stream, char *file, size_t file_cap)
{
    size_t nlen;
    const char *dot;
    const char *p;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    if (!path_ends_with_ci(name, ".m4s")) {
        return 0;
    }
    nlen = strlen(name);
    if (nlen < 6) {
        return 0;
    }
    dot = name + nlen - 4;
    if (dot[0] != '.' || dot[1] != 'm' || dot[2] != '4' || dot[3] != 's') {
        return 0;
    }
    if (dot == name || dot[-1] < '0' || dot[-1] > '9') {
        return 0;
    }
    p = dot - 1;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') {
        --p;
    }
    if (p <= name || p[-1] != '.') {
        return 0;
    }
    {
        size_t slen = (size_t)(p - 1 - name);
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
    }
    strncpy(file, p, file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

/** 直播 TS {app}/{stream}.{N}.ts；stream + 内部文件 {N}.ts */
/* 反推 stream 的方式是从扩展名往前吃掉连续数字，再要求其前一位是 '.'——
 * 这样 "abc.12.ts" 得 stream="abc"、file="12.ts"，而 "abc.ts"（无数字段）
 * 因数字段前不是 '.' 而不被误判。m4s 版逻辑同构，只是扩展名不同。 */
static int parse_live_hls_ts_name(const char *name, char *stream, char *file, size_t file_cap)
{
    size_t nlen;
    const char *ts_dot;
    const char *p;

    if (!name || !stream || !file || file_cap == 0) {
        return 0;
    }
    stream[0] = '\0';
    file[0] = '\0';
    if (!path_ends_with_ci(name, ".ts")) {
        return 0;
    }
    nlen = strlen(name);
    if (nlen < 5) {
        return 0;
    }
    ts_dot = name + nlen - 3;
    if (ts_dot[0] != '.' || ts_dot[1] != 't' || ts_dot[2] != 's') {
        return 0;
    }
    if (ts_dot == name || ts_dot[-1] < '0' || ts_dot[-1] > '9') {
        return 0;
    }
    p = ts_dot - 1;
    while (p > name && p[-1] >= '0' && p[-1] <= '9') {
        --p;
    }
    if (p <= name || p[-1] != '.') {
        return 0;
    }
    {
        size_t slen = (size_t)(p - 1 - name);
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
    }
    strncpy(file, p, file_cap - 1);
    file[file_cap - 1] = '\0';
    return 1;
}

static int hls_line_is_plain_seg(const char *line, size_t len)
{
    char buf[64];
    size_t seg_len = len;

    if (!line || len < 4) {
        return 0;
    }
    while (seg_len > 0 && line[seg_len - 1] == '\r') {
        --seg_len;
    }
    if (seg_len >= sizeof(buf)) {
        return 0;
    }
    memcpy(buf, line, seg_len);
    buf[seg_len] = '\0';
#if defined(_WIN32)
    if (_stricmp(buf, "init.mp4") == 0)
#else
    if (strcasecmp(buf, "init.mp4") == 0)
#endif
        return 1;
    return hls_seg_file_is_index_ts(buf) || hls_seg_file_is_index_m4s(buf);
}

static int hls_line_rewrite_map_uri(char *line, size_t cap, const char *prefix, size_t prefix_len)
{
    const char *uri = strstr(line, "URI=\"");
    char *end;
    char new_uri[ZMS_APP_MAX + 1 + ZMS_STREAM_MAX + 32];
    size_t uri_len;
    int n;

    if (!uri || !prefix || prefix_len == 0 || cap < 32) {
        return 0;
    }
    uri += 5;
    end = strchr(uri, '\"');
    if (!end) {
        return 0;
    }
    uri_len = (size_t)(end - uri);
    if (uri_len >= sizeof(new_uri) - prefix_len - 1) {
        return 0;
    }
    memcpy(new_uri, prefix, prefix_len);
    memcpy(new_uri + prefix_len, uri, uri_len);
    new_uri[prefix_len + uri_len] = '\0';
    n = snprintf(line, cap, "#EXT-X-MAP:URI=\"%s\",\r\n", new_uri);
    return n > 0 && (size_t)n < cap;
}

/*
 * 重写直播 m3u8：把分片行（纯数字索引）与 #EXT-X-MAP 的 URI 加上
 * "/{app}/{stream}." 前缀。
 *
 * 为什么需要：服务端对外把 playlist 暴露在不同路径下，而生成 playlists 时
 * 分片 URI 是相对自身写的。若直接把原文件吐出去，播放器会相对 playlist 的
 * URL 去拼分片地址，可能落到错误的路由。统一加前缀后，分片请求能精确落到
 * {app}/{stream}.{N}.ts 的解析分支。
 * #EXT-X-MAP 内的 init 分片 URI 也要改，否则 fMP4 拿不到初始化段。
 */
size_t zms_http_route_rewrite_live_hls_m3u8(char *buf, size_t cap, const char *app,
                                            const char *stream)
{
    char prefix[ZMS_APP_MAX + 1 + ZMS_STREAM_MAX + 2];
    char *tmp;
    size_t tmp_cap;
    const char *read;
    char *write;
    char *end;
    size_t prefix_len;
    size_t out_len = 0;
    int n;

    if (!buf || cap == 0 || !app || !app[0] || !stream || !stream[0]) {
        return buf ? strlen(buf) : 0;
    }

    n = snprintf(prefix, sizeof(prefix), "/%s/%s.", app, stream);
    if (n <= 0 || (size_t)n >= sizeof(prefix)) {
        return strlen(buf);
    }
    prefix_len = (size_t)n;

    /* 结果长度 = 原文本 + 每个分片行新增的 prefix：按实际需要分配，
     * 不能固定 8KB（长点播 playlist 会被截断），也不能按 cap 一次吃满。 */
    {
        size_t text_len = strlen(buf);
        size_t lines = 1;
        size_t need;
        const char *p;

        for (p = buf; *p; ++p) {
            if (*p == '\n') {
                ++lines;
            }
        }
        need = text_len + lines * prefix_len + 1;
        if (need > cap) {
            need = cap;
        }
        if (need < 2) {
            return text_len;
        }
        tmp = (char *)malloc(need);
        if (!tmp) {
            return text_len;
        }
        tmp_cap = need;
    }

    read = buf;
    write = tmp;
    end = tmp + tmp_cap;
    while (*read && write < end) {
        char *nl = strchr(read, '\n');
        size_t line_len = nl ? (size_t)(nl - read) : strlen(read);
        size_t seg_len = line_len;

        while (seg_len > 0 && read[seg_len - 1] == '\r') {
            --seg_len;
        }

        if (hls_line_is_plain_seg(read, seg_len)) {
            if ((size_t)(write - tmp) + prefix_len + seg_len + (nl ? 1 : 0) + 1 >
                (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, prefix, prefix_len);
            write += prefix_len;
            memcpy(write, read, seg_len);
            write += seg_len;
        } else if (seg_len > 12 && strncmp(read, "#EXT-X-MAP:", 11) == 0) {
            char map_line[256];
            if (seg_len >= sizeof(map_line)) {
                seg_len = sizeof(map_line) - 1;
            }
            memcpy(map_line, read, seg_len);
            map_line[seg_len] = '\0';
            if (hls_line_rewrite_map_uri(map_line, sizeof(map_line), prefix, prefix_len)) {
                size_t map_len = strlen(map_line);
                if ((size_t)(write - tmp) + map_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                    break;
                }
                memcpy(write, map_line, map_len);
                write += map_len;
            } else {
                if ((size_t)(write - tmp) + line_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                    break;
                }
                memcpy(write, read, line_len);
                write += line_len;
            }
        } else {
            if ((size_t)(write - tmp) + line_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, read, line_len);
            write += line_len;
        }
        if (nl) {
            *write++ = '\n';
            read = nl + 1;
        } else {
            read += line_len;
        }
    }
    *write = '\0';
    out_len = (size_t)(write - tmp);
    if (out_len >= cap) {
        out_len = cap - 1;
    }
    memcpy(buf, tmp, out_len);
    buf[out_len] = '\0';
    free(tmp);
    return out_len;
}

/* 判断路径 rel 是否为绝对路径分段 URI（点播 HLS 分片行）。排除以 '#'(注释)、
 * '/'(绝对路径) 开头的行，并校验字符集只含合法 URI 字符、结尾必须是 .ts，
 * 从而避免把 playlist 的注释/标签行误当分片去加前缀。 */
static int vod_hls_line_is_seg_uri(const char *line, size_t len)
{
    size_t i;

    if (!line || len < 4) {
        return 0;
    }
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        --len;
    }
    if (len < 4 || line[0] == '#' || line[0] == '/') {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        char c = line[i];
        if (c == '/') {
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            continue;
        }
        return 0;
    }
    return (line[len - 3] == '.' && (line[len - 2] == 't' || line[len - 2] == 'T') &&
            (line[len - 1] == 's' || line[len - 1] == 'S'));
}

/*
 * 重写点播 m3u8：给相对分片 URI 补上 "/{app}/{m3u8 所在目录}/" 前缀。
 * 与直播版的关键差异：前缀要带上 m3u8 自身的子目录（dir），因为点播
 * playlist 与分片可能不在根目录下；只补 /{app}/ 会丢掉中间的路径层级。
 */
size_t zms_http_route_rewrite_vod_hls_m3u8(char *buf, size_t cap, const char *app,
                                           const char *m3u8_rel)
{
    char prefix[ZMS_CFG_PATH_MAX];
    char *tmp;
    size_t tmp_cap;
    const char *read;
    char *write;
    char *end;
    size_t prefix_len;
    size_t out_len = 0;
    const char *dir = "";
    char dirbuf[ZMS_STREAM_MAX];
    const char *slash;
    int n;

    if (!buf || cap == 0 || !app || !app[0]) {
        return buf ? strlen(buf) : 0;
    }

    if (m3u8_rel && m3u8_rel[0]) {
        slash = strrchr(m3u8_rel, '/');
        if (slash && slash > m3u8_rel) {
            size_t dlen = (size_t)(slash - m3u8_rel);
            if (dlen >= sizeof(dirbuf)) {
                dlen = sizeof(dirbuf) - 1;
            }
            memcpy(dirbuf, m3u8_rel, dlen);
            dirbuf[dlen] = '\0';
            dir = dirbuf;
        }
    }

    if (dir[0]) {
        n = snprintf(prefix, sizeof(prefix), "/%s/%s/", app, dir);
    } else {
        n = snprintf(prefix, sizeof(prefix), "/%s/", app);
    }
    if (n <= 0 || (size_t)n >= sizeof(prefix)) {
        return strlen(buf);
    }
    prefix_len = (size_t)n;

    /* 结果长度 = 原文 + 每个分片行新增的 prefix：按实际需要分配，
     * 固定 8KB 会截断长 playlist，按调用方 cap 直接吃满又太浪费。 */
    {
        size_t text_len = strlen(buf);
        size_t lines = 1;
        size_t need;
        const char *p;

        for (p = buf; *p; ++p) {
            if (*p == '\n') {
                ++lines;
            }
        }
        need = text_len + lines * prefix_len + 1;
        if (need > cap) {
            need = cap;
        }
        if (need < 2) {
            return text_len;
        }
        tmp = (char *)malloc(need);
        if (!tmp) {
            return text_len;
        }
        tmp_cap = need;
    }

    read = buf;
    write = tmp;
    end = tmp + tmp_cap;
    while (*read && write < end) {
        char *nl = strchr(read, '\n');
        size_t line_len = nl ? (size_t)(nl - read) : strlen(read);
        size_t seg_len = line_len;

        while (seg_len > 0 && read[seg_len - 1] == '\r') {
            --seg_len;
        }

        if (vod_hls_line_is_seg_uri(read, seg_len)) {
            if ((size_t)(write - tmp) + prefix_len + seg_len + (nl ? 1 : 0) + 1 >
                (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, prefix, prefix_len);
            write += prefix_len;
            memcpy(write, read, seg_len);
            write += seg_len;
        } else {
            if ((size_t)(write - tmp) + line_len + (nl ? 1 : 0) + 1 > (size_t)(end - tmp)) {
                break;
            }
            memcpy(write, read, line_len);
            write += line_len;
        }
        if (nl) {
            *write++ = '\n';
            read = nl + 1;
        } else {
            read += line_len;
        }
    }
    *write = '\0';
    out_len = (size_t)(write - tmp);
    if (out_len >= cap) {
        out_len = cap - 1;
    }
    memcpy(buf, tmp, out_len);
    buf[out_len] = '\0';
    free(tmp);
    return out_len;
}

/*
 * 解析直播 HLS 路径。开头先挡掉点播 app 下的 *.m3u8/*.ts：点播 HLS 由
 * zms_http_route_parse_vod_hls_path 处理（需要额外返回 rel 供读盘），这里的
 * 直播解析器一旦放到点播 app 上会把文件路径错当流名，故提前返回 0。
 * 之后按 ZLM 兼容 → {stream}.m3u8 → 数字分片/init 的优先级依次尝试。
 */
int zms_http_route_parse_hls_path(const char *path, char *app, char *stream, char *file,
                                  size_t file_cap)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (file_cap && file) {
        file[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    /* 点播 HLS 路由：{app}/*.m3u8|*.ts */
    {
        const char *pp = path_only + 1;
        const char *ps = strchr(pp, '/');
        if (ps) {
            char app_probe[ZMS_APP_MAX];
            size_t alen = (size_t)(ps - pp);
            if (alen >= ZMS_APP_MAX) {
                alen = ZMS_APP_MAX - 1;
            }
            memcpy(app_probe, pp, alen);
            app_probe[alen] = '\0';
            if (zms_vod_is_record_app(app_probe)) {
                const char *tail = ps + 1;
                if (path_ends_with_ci(tail, ".m3u8") || path_ends_with_ci(tail, ".ts")) {
                    return 0;
                }
            }
        }
    }

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }
    if (zms_vod_is_record_app(app)) {
        return 0;
    }

    name = slash + 1;
    if (!name[0]) {
        return 0;
    }

    /* ZLM 兼容：{app}/{stream}/hls.m3u8 */
    if (path_ends_with_ci(name, "/hls.m3u8")) {
        size_t nlen = strlen(name);
        size_t slen = nlen - 9;
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        if (file_cap && file) {
            strncpy(file, "hls.m3u8", file_cap - 1);
        }
        return 1;
    }

    /* 直播：{app}/{stream}.m3u8（stream 可含 /）*/
    if (path_ends_with_ci(name, ".m3u8")) {
        size_t slen = strlen(name) - 5;
        if (slen == 0 || slen >= ZMS_STREAM_MAX) {
            return 0;
        }
        memcpy(stream, name, slen);
        stream[slen] = '\0';
        if (file_cap && file) {
            strncpy(file, name, file_cap - 1);
        }
        return 1;
    }

    /* 直播：{app}/{stream}.{N}.ts / {N}.m4s / init.mp4 */
    if (parse_live_hls_ts_name(name, stream, file, file_cap)) {
        return 1;
    }
    if (parse_live_hls_m4s_name(name, stream, file, file_cap)) {
        return 1;
    }
    if (parse_live_hls_init_name(name, stream, file, file_cap)) {
        return 1;
    }
    if (parse_live_hls_ts_slash_name(name, stream, file, file_cap)) {
        return 1;
    }

    return 0;
}

/*
 * 解析点播 MP4 直连路径 {app}/{stream}.mp4（HTTP 直接播 mp4 文件）。
 * 最后一步 zms_vod_rel_path_safe 是安全闸：stream 直接来自 URL，必须挡掉
 * ".." 与绝对路径，否则可穿越到 record_root 之外读任意文件。
 */
int zms_http_route_parse_mp4_path(const char *path, char *app, char *stream)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;
    size_t slen;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';
    if (!path_ends_with_ci(path_only, ".mp4")) {
        return 0;
    }

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }

    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }

    name = slash + 1;
    if (!name[0] || !path_ends_with_ci(name, ".mp4")) {
        return 0;
    }
    slen = strlen(name);
    if (slen >= ZMS_STREAM_MAX) {
        slen = ZMS_STREAM_MAX - 1;
    }
    memcpy(stream, name, slen);
    stream[slen] = '\0';
    if (!zms_vod_rel_path_safe(stream)) {
        return 0;
    }
    return 1;
}

/*
 * 解析 DASH 的 mpd/扁平 m4s 文件名，反推 stream。
 *   - .mpd：直接去掉扩展名；
 *   - "{stream}-init.mp4"：去掉 "-init.xxx"；
 *   - "{stream}-{数字}.m4s"：去掉末尾 "-数字.扩展名"。
 * 与 HLS 不同，DASH 的 stream 与分片序号之间用 '-' 分隔，故这里反向吃掉
 * 连续数字后要求前一位是 '-'，而不是 HLS 的 '.'。
 */
static int dash_flat_stream_from_fname(const char *fname, char *stream, size_t stream_cap)
{
    const char *dot;
    const char *p;
    size_t slen;

    if (!fname || !fname[0] || !stream || stream_cap == 0) {
        return 0;
    }

    if (path_ends_with_ci(fname, ".mpd")) {
        slen = strlen(fname) - 4;
        if (slen == 0 || slen >= stream_cap) {
            return 0;
        }
        memcpy(stream, fname, slen);
        stream[slen] = '\0';
        return 1;
    }

    dot = strrchr(fname, '.');
    if (!dot || dot == fname) {
        return 0;
    }

    if ((size_t)(dot - fname) >= 5 && strncmp(dot - 5, "-init", 5) == 0) {
        slen = (size_t)(dot - 5 - fname);
    } else {
        p = dot - 1;
        while (p > fname && *p >= '0' && *p <= '9') {
            --p;
        }
        if (p == fname || *p != '-') {
            return 0;
        }
        slen = (size_t)(p - fname);
    }
    if (slen == 0 || slen >= stream_cap) {
        return 0;
    }
    memcpy(stream, fname, slen);
    stream[slen] = '\0';
    return 1;
}

/*
 * 解析 DASH 路径 {app}/{name}，name 只允许单层（不含 '/'）。支持的扩展名有
 * mpd / m4s / m4v / m4a；不支持嵌套子目录是刻意的——DASH 分片以扁平文件名
 * 平铺，出现子目录说明不是本端生成的布局。点播 app 一律拒绝（DASH 只做直播）。
 */
int zms_http_route_parse_dash_path(const char *path, char *app, char *stream, char *file,
                                   size_t file_cap)
{
    char path_only[512];
    const char *q;
    size_t plen;
    const char *p;
    const char *slash;
    const char *name;

    if (app) {
        app[0] = '\0';
    }
    if (stream) {
        stream[0] = '\0';
    }
    if (file_cap && file) {
        file[0] = '\0';
    }
    if (!path || path[0] != '/' || !app || !stream) {
        return 0;
    }

    q = strchr(path, '?');
    plen = q ? (size_t)(q - path) : strlen(path);
    if (plen >= sizeof(path_only)) {
        plen = sizeof(path_only) - 1;
    }
    memcpy(path_only, path, plen);
    path_only[plen] = '\0';

    p = path_only + 1;
    slash = strchr(p, '/');
    if (!slash) {
        return 0;
    }
    {
        size_t alen = (size_t)(slash - p);
        if (alen >= ZMS_APP_MAX) {
            alen = ZMS_APP_MAX - 1;
        }
        memcpy(app, p, alen);
        app[alen] = '\0';
    }
    if (zms_vod_is_record_app(app)) {
        return 0;
    }

    name = slash + 1;
    if (!name[0] || strchr(name, '/')) {
        return 0;
    }

    if (!path_ends_with_ci(name, ".mpd") && !path_ends_with_ci(name, ".m4s") &&
        !path_ends_with_ci(name, ".m4v") && !path_ends_with_ci(name, ".m4a")) {
        return 0;
    }
    if (!dash_flat_stream_from_fname(name, stream, ZMS_STREAM_MAX)) {
        return 0;
    }
    if (file_cap && file) {
        strncpy(file, name, file_cap - 1);
    }
    return 1;
}
