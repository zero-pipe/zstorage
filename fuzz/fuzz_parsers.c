/**
 * @file fuzz_parsers.c
 * @brief libFuzzer 靶：外部输入解析路径（HTTP / RTSP / SIP / XML / RTMP）。
 *
 * 每个靶都把输入当作一条完整报文喂给对应解析器，只验证「不崩溃、不越界、
 * 不泄漏」，不校验业务语义。用 libFuzzer 的调试构建单独编译：
 *
 *   cc -g -O1 -fsanitize=fuzzer,address,undefined \
 *      -Iinclude -Isrc -I3rdpart/znet/include -I3rdpart/zbase/include \
 *      fuzz/fuzz_parsers.c ... -o fuzz_parsers
 *   ./fuzz_parsers -max_total_time=300
 *
 * 选择靶：命令行参数 fuzz 驱动默认走全部；也可编译期 -DFUZZ_TARGET=xxx，
 * 或运行时用输入首字节选择（见 fuzz_dispatch）。
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "zms/protocol/http/parser.h"
#include "zms/protocol/http/request_reader.h"
#include "zms/protocol/rtsp/parser.h"
#include "zms/protocol/rtmp/protocol.h"
#include "zms/message/xml.h"
#include "zn/util/log.h"
#include "device/gb28181/gb28181_internal.h"

/* ── HTTP 报文解析 ─────────────────────────────────────────────────────── */
static void fuzz_http(const uint8_t *data, size_t size)
{
    zms_http_parser p;

    zms_http_parser_init(&p);
    (void)zms_http_parser_parse(&p, (const char *)data, size);
    /* 顺带读一次头与 query，覆盖访问器 */
    (void)zms_http_parser_header(&p, "Content-Length");
    (void)zms_http_parser_header(&p, "Range");
    {
        char url[ZMS_HTTP_URL_MAX];
        (void)zms_http_parser_full_url(&p, url, sizeof(url));
    }
    zms_http_parser_clear(&p);
}

/* ── HTTP 请求分帧（splitter + request_reader），按块喂入以覆盖增量路径 ── */
static void on_req(const zms_http_request *req, void *user)
{
    (void)req;
    (void)user;
}

static void fuzz_http_reader(const uint8_t *data, size_t size)
{
    zms_http_request_reader_opts opts = {.on_request = on_req, .user = NULL};
    zms_http_request_reader *rd = zms_http_request_reader_create(&opts);
    size_t off = 0;

    if (!rd) {
        return;
    }
    /* 按 1..8 字节块喂入，覆盖跨块的增量解析 */
    while (off < size) {
        size_t chunk = (data[off] % 8) + 1;
        if (off + chunk > size) {
            chunk = size - off;
        }
        (void)zms_http_request_reader_input(rd, data + off, chunk);
        off += chunk;
    }
    zms_http_request_reader_destroy(rd);
}

/* ── RTSP 消息解析 ─────────────────────────────────────────────────────── */
static void fuzz_rtsp(const uint8_t *data, size_t size)
{
    zms_rtsp_message msg;

    memset(&msg, 0, sizeof(msg));
    zms_msg_init(&msg.msg);
    {
        size_t consumed = 0;
        (void)zms_rtsp_message_parse((const char *)data, size, &msg, &consumed);
    }
    (void)zms_rtsp_message_get(&msg, "CSeq");
    (void)zms_rtsp_message_content_length(&msg);
    zms_rtsp_message_clear(&msg);
}

/* ── SIP 消息解析（GB28181 设备/平台信令面）────────────────────────────── */
static void fuzz_sip(const uint8_t *data, size_t size)
{
    gb_sip_msg m;

    memset(&m, 0, sizeof(m));
    (void)gb_sip_parse(data, size, "127.0.0.1", 5060, &m);
    (void)gb_sip_hdr(&m, "Call-ID");
    {
        char user[64];
        gb_sip_uri_user(gb_sip_hdr(&m, "To"), user, sizeof(user));
    }
}

/* ── XML / MANSCDP 文本助手 ────────────────────────────────────────────── */
static void fuzz_xml(const uint8_t *data, size_t size)
{
    char buf[4096];
    size_t n = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;

    memcpy(buf, data, n);
    buf[n] = '\0';
    {
        char out[256];
        (void)zms_xml_find(buf, "DeviceID");
        (void)zms_xml_text(buf, "CmdType", out, sizeof(out));
        (void)zms_xml_attr(buf, "Item", "Name", out, sizeof(out));
        (void)zms_xml_local_name(buf, out, sizeof(out));
    }
}

/* ── RTMP chunk 流解析 ─────────────────────────────────────────────────── */
static void on_chunk(const zms_rtmp_chunk *chunk, void *user)
{
    (void)chunk;
    (void)user;
}

static void fuzz_rtmp(const uint8_t *data, size_t size)
{
    zms_rtmp_protocol_opts opts = {.on_chunk = on_chunk, .on_cmd = NULL, .user = NULL};
    zms_rtmp_protocol *p = zms_rtmp_protocol_create_established(&opts);

    if (!p) {
        return;
    }
    (void)zms_rtmp_protocol_input(p, data, size);
    zms_rtmp_protocol_destroy(p);
}

/* ── 入口：首字节选择靶，其余作为输入 ──────────────────────────────────── */
/* libFuzzer 每次调用都可能重新初始化进程状态；日志调到 ERROR 以降低噪音。 */
static void fuzz_init_once(void)
{
    static int done;

    if (!done) {
        done = 1;
        zn_log_set_level(ZN_LOG_ERROR);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t sel;

    fuzz_init_once();
    if (size < 1) {
        return 0;
    }
    sel = data[0] % 6;
    data += 1;
    size -= 1;
    switch (sel) {
    case 0:
        fuzz_http(data, size);
        break;
    case 1:
        fuzz_http_reader(data, size);
        break;
    case 2:
        fuzz_rtsp(data, size);
        break;
    case 3:
        fuzz_sip(data, size);
        break;
    case 4:
        fuzz_xml(data, size);
        break;
    case 5:
        fuzz_rtmp(data, size);
        break;
    default:
        break;
    }
    return 0;
}
