/**
 * @file gb_manscdp.c
 * @brief MANSCDP XML（GB28181 信令体）解析与构造。
 */
#include "device/gb28181/gb28181_internal.h"
#include "zms/device/core/device_xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32) && defined(_MSC_VER)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#endif

static int tag_text(const char *body, const char *tag, char *out, size_t cap)
{
    return zms_xml_text(body, tag, out, cap) == 0;
}

static int tag_int(const char *body, const char *tag, int *out)
{
    char tmp[32];
    if (!tag_text(body, tag, tmp, sizeof(tmp))) {
        return 0;
    }
    *out = atoi(tmp);
    return 1;
}

int gb_manscdp_parse(const char *body, size_t len, gb_manscdp_msg *out)
{
    static const char *roots[] = {"Notify", "Response", "Query", "Control"};
    size_t i;

    if (!body || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    (void)i;
    zms_msg_init(&out->msg);
    (void)zms_msg_set_body(&out->msg, body, len);

    if (zms_msg_xml_root(&out->msg, roots, (int)(sizeof(roots) / sizeof(roots[0])), out->root,
                         sizeof(out->root)) != 0) {
        return -1;
    }
    (void)zms_msg_xml_get(&out->msg, "CmdType", out->cmd_type, sizeof(out->cmd_type));
    (void)zms_msg_xml_get(&out->msg, "SN", out->sn, sizeof(out->sn));
    (void)zms_msg_xml_get(&out->msg, "DeviceID", out->device_id, sizeof(out->device_id));
    (void)zms_msg_xml_get(&out->msg, "Result", out->result, sizeof(out->result));
    (void)zms_msg_xml_get(&out->msg, "PTZCmd", out->ptz_cmd, sizeof(out->ptz_cmd));
    (void)zms_msg_xml_get(&out->msg, "RecordCmd", out->record_cmd, sizeof(out->record_cmd));
    (void)zms_msg_xml_get(&out->msg, "GuardCmd", out->guard_cmd, sizeof(out->guard_cmd));
    (void)zms_msg_xml_get(&out->msg, "StartTime", out->start_time, sizeof(out->start_time));
    tag_text(body, "EndTime", out->end_time, sizeof(out->end_time));
    tag_text(body, "Type", out->type, sizeof(out->type));
    tag_int(body, "Secrecy", &out->secrecy);
    tag_int(body, "SumNum", &out->sum_num);
    if (!out->cmd_type[0]) {
        return -1;
    }
    return 0;
}

static void xml_decl(gb_sb *w)
{
    gb_sb_puts(w, "<?xml version=\"1.0\" encoding=\"GB2312\"?>\r\n");
}

int gb_manscdp_get(const gb_manscdp_msg *q, const char *tag, char *out, size_t cap)
{
    if (!q) {
        return -1;
    }
    return zms_msg_xml_get(&q->msg, tag, out, cap);
}

int gb_manscdp_build_keepalive(char *out, size_t cap, const char *device_id, const char *sn)
{
    gb_sb w;

    gb_sb_init(&w, out, cap);
    xml_decl(&w);
    gb_sb_puts(&w, "<Notify>\r\n<CmdType>Keepalive</CmdType>\r\n");
    gb_sb_printf(&w, "<SN>%s</SN>\r\n", sn ? sn : "1");
    gb_sb_printf(&w, "<DeviceID>%s</DeviceID>\r\n", device_id ? device_id : "");
    gb_sb_puts(&w, "<Status>OK</Status>\r\n</Notify>\r\n");
    return (int)w.len;
}

int gb_manscdp_build_device_info(char *out, size_t cap, const char *sn, const char *device_id,
                                 const char *device_name, const char *manufacturer,
                                 const char *model, const char *firmware, int channel_count)
{
    gb_sb w;

    gb_sb_init(&w, out, cap);
    xml_decl(&w);
    gb_sb_puts(&w, "<Response>\r\n<CmdType>DeviceInfo</CmdType>\r\n");
    gb_sb_printf(&w, "<SN>%s</SN>\r\n<DeviceID>%s</DeviceID>\r\n", sn ? sn : "",
                 device_id ? device_id : "");
    gb_sb_puts(&w, "<Result>OK</Result>\r\n<DeviceName>");
    gb_sb_xml_esc(&w, device_name ? device_name : "");
    gb_sb_puts(&w, "</DeviceName>\r\n<Manufacturer>");
    gb_sb_xml_esc(&w, manufacturer ? manufacturer : "");
    gb_sb_puts(&w, "</Manufacturer>\r\n<Model>");
    gb_sb_xml_esc(&w, model ? model : "");
    gb_sb_puts(&w, "</Model>\r\n<Firmware>");
    gb_sb_xml_esc(&w, firmware ? firmware : "");
    gb_sb_printf(&w, "</Firmware>\r\n<Channel>%d</Channel>\r\n</Response>\r\n", channel_count);
    return (int)w.len;
}

int gb_manscdp_build_device_status(char *out, size_t cap, const char *sn, const char *device_id,
                                   int online)
{
    gb_sb w;
    char now[40];

    gb_format_time_iso(time(NULL), now, sizeof(now));
    gb_sb_init(&w, out, cap);
    xml_decl(&w);
    gb_sb_puts(&w, "<Response>\r\n<CmdType>DeviceStatus</CmdType>\r\n");
    gb_sb_printf(&w, "<SN>%s</SN>\r\n<DeviceID>%s</DeviceID>\r\n", sn ? sn : "",
                 device_id ? device_id : "");
    gb_sb_puts(&w, "<Result>OK</Result>\r\n");
    gb_sb_printf(&w, "<Online>%s</Online>\r\n", online ? "ONLINE" : "OFFLINE");
    gb_sb_puts(&w, "<Status>OK</Status>\r\n<Encode>ON</Encode>\r\n<Record>ON</Record>\r\n");
    gb_sb_printf(&w, "<DeviceTime>%s</DeviceTime>\r\n</Response>\r\n", now);
    return (int)w.len;
}

int gb_manscdp_build_device_control_resp(char *out, size_t cap, const char *sn,
                                         const char *device_id, const char *result)
{
    gb_sb w;

    gb_sb_init(&w, out, cap);
    xml_decl(&w);
    gb_sb_puts(&w, "<Response>\r\n<CmdType>DeviceControl</CmdType>\r\n");
    gb_sb_printf(&w, "<SN>%s</SN>\r\n<DeviceID>%s</DeviceID>\r\n", sn ? sn : "",
                 device_id ? device_id : "");
    gb_sb_printf(&w, "<Result>%s</Result>\r\n</Response>\r\n", result ? result : "OK");
    return (int)w.len;
}

int gb_manscdp_build_catalog(char *out, size_t cap, const char *sn, const char *device_id,
                             const gb_catalog_item *items, size_t count)
{
    gb_sb w;
    size_t i;

    gb_sb_init(&w, out, cap);
    xml_decl(&w);
    gb_sb_puts(&w, "<Response>\r\n<CmdType>Catalog</CmdType>\r\n");
    gb_sb_printf(&w, "<SN>%s</SN>\r\n<DeviceID>%s</DeviceID>\r\n", sn ? sn : "",
                 device_id ? device_id : "");
    gb_sb_printf(&w, "<SumNum>%u</SumNum>\r\n<DeviceList Num=\"%u\">\r\n", (unsigned)count,
                 (unsigned)count);
    for (i = 0; i < count; ++i) {
        const gb_catalog_item *it = &items[i];
        gb_sb_puts(&w, "<Item>\r\n");
        gb_sb_printf(&w, "<DeviceID>%s</DeviceID>\r\n", it->device_id ? it->device_id : "");
        gb_sb_puts(&w, "<Name>");
        gb_sb_xml_esc(&w, it->name ? it->name : (it->device_id ? it->device_id : ""));
        gb_sb_puts(&w, "</Name>\r\n<Manufacturer>");
        gb_sb_xml_esc(&w, it->manufacturer ? it->manufacturer : "");
        gb_sb_puts(&w, "</Manufacturer>\r\n<Model>");
        gb_sb_xml_esc(&w, it->model ? it->model : "");
        gb_sb_puts(&w, "</Model>\r\n<Owner>Owner</Owner>\r\n<CivilCode>340200</CivilCode>\r\n");
        gb_sb_puts(&w, "<Address>Address</Address>\r\n<Parental>0</Parental>\r\n");
        gb_sb_printf(&w, "<ParentID>%s</ParentID>\r\n", it->parent_id ? it->parent_id : "");
        gb_sb_puts(&w, "<RegisterWay>1</RegisterWay>\r\n<Secrecy>0</Secrecy>\r\n");
        gb_sb_printf(&w, "<Status>%s</Status>\r\n", it->online ? "ON" : "OFF");
        gb_sb_puts(&w, "<Longitude>0.0</Longitude>\r\n<Latitude>0.0</Latitude>\r\n");
        if (it->ptz_type > 0) {
            gb_sb_printf(&w, "<Info><PTZType>%d</PTZType></Info>\r\n", it->ptz_type);
        }
        gb_sb_puts(&w, "</Item>\r\n");
    }
    gb_sb_puts(&w, "</DeviceList>\r\n</Response>\r\n");
    return (int)w.len;
}

int gb_manscdp_build_record_info(char *out, size_t cap, const char *sn, const char *device_id,
                                 const gb_record_item *items, size_t count, int sum_num)
{
    gb_sb w;
    size_t i;

    gb_sb_init(&w, out, cap);
    xml_decl(&w);
    gb_sb_puts(&w, "<Response>\r\n<CmdType>RecordInfo</CmdType>\r\n");
    gb_sb_printf(&w, "<SN>%s</SN>\r\n<DeviceID>%s</DeviceID>\r\n", sn ? sn : "",
                 device_id ? device_id : "");
    gb_sb_printf(&w, "<SumNum>%d</SumNum>\r\n<RecordList Num=\"%u\">\r\n",
                 sum_num > 0 ? sum_num : (int)count, (unsigned)count);
    for (i = 0; i < count; ++i) {
        const gb_record_item *it = &items[i];
        gb_sb_puts(&w, "<Item>\r\n");
        gb_sb_printf(&w, "<DeviceID>%s</DeviceID>\r\n", it->device_id ? it->device_id : "");
        gb_sb_puts(&w, "<Name>");
        gb_sb_xml_esc(&w, it->name ? it->name : "");
        gb_sb_puts(&w, "</Name>\r\n<FilePath>");
        gb_sb_xml_esc(&w, it->file_path ? it->file_path : "");
        gb_sb_puts(&w, "</FilePath>\r\n<Address>Address</Address>\r\n");
        gb_sb_printf(&w, "<StartTime>%s</StartTime>\r\n<EndTime>%s</EndTime>\r\n",
                     it->start_time ? it->start_time : "", it->end_time ? it->end_time : "");
        gb_sb_puts(&w, "<Secrecy>0</Secrecy>\r\n");
        gb_sb_printf(&w, "<Type>%s</Type>\r\n", it->type && it->type[0] ? it->type : "time");
        gb_sb_puts(&w, "</Item>\r\n");
    }
    gb_sb_puts(&w, "</RecordList>\r\n</Response>\r\n");
    return (int)w.len;
}

int gb_manscdp_build_alarm(char *out, size_t cap, const char *sn, const char *device_id,
                           int priority, int method, const char *description)
{
    gb_sb w;
    char now[40];

    gb_format_time_iso(time(NULL), now, sizeof(now));
    gb_sb_init(&w, out, cap);
    xml_decl(&w);
    gb_sb_puts(&w, "<Notify>\r\n<CmdType>Alarm</CmdType>\r\n");
    gb_sb_printf(&w, "<SN>%s</SN>\r\n<DeviceID>%s</DeviceID>\r\n", sn ? sn : "",
                 device_id ? device_id : "");
    gb_sb_printf(&w, "<AlarmPriority>%d</AlarmPriority>\r\n<AlarmMethod>%d</AlarmMethod>\r\n",
                 priority > 0 ? priority : 1, method > 0 ? method : 5);
    gb_sb_printf(&w, "<AlarmTime>%s</AlarmTime>\r\n", now);
    gb_sb_puts(&w, "<AlarmDescription>");
    gb_sb_xml_esc(&w, description ? description : "alarm");
    gb_sb_puts(&w, "</AlarmDescription>\r\n</Notify>\r\n");
    return (int)w.len;
}

void gb_format_time_iso(time_t t, char *out, size_t cap)
{
    struct tm tmv;

    if (!out || cap == 0) {
        return;
    }
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    strftime(out, cap, "%Y-%m-%dT%H:%M:%S", &tmv);
}
