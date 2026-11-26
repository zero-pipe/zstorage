/**
 * @file test_gb28181_sip.c
 * @brief GB28181 信令层：SIP 解析/构造、Digest、MANSCDP。
 */
#include "device/gb28181/gb28181_internal.h"
#include "zms/util/md5.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "check failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__);              \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int test_sip_roundtrip(void)
{
    char buf[4096];
    gb_sip_req_spec spec;
    gb_sip_msg m;
    int n;

    memset(&spec, 0, sizeof(spec));
    spec.method = "REGISTER";
    spec.uri = "sip:34020000002000000001@3402000000";
    spec.from_user = "34020000001320000001";
    spec.from_host = "3402000000";
    spec.from_tag = "abcdef";
    spec.to_user = "34020000002000000001";
    spec.to_host = "3402000000";
    spec.call_id = "call-1";
    spec.cseq = 7;
    spec.local_ip = "192.168.1.10";
    spec.local_port = 5060;
    spec.contact_user = "34020000001320000001";
    spec.has_expires = 1;
    spec.expires = 3600;

    n = gb_sip_build_request(buf, sizeof(buf), &spec);
    CHECK(n > 0);
    CHECK(gb_sip_parse(buf, (size_t)n, "192.168.1.100", 5060, &m) == 0);
    CHECK(!m.is_response);
    CHECK(strcmp(m.method, "REGISTER") == 0);
    CHECK(strcmp(m.uri, spec.uri) == 0);
    CHECK(strcmp(gb_sip_hdr(&m, "Call-ID"), "call-1") == 0);
    CHECK(strcmp(gb_sip_hdr(&m, "CSeq"), "7 REGISTER") == 0);

    {
        const char *raw = "MESSAGE sip:x@y SIP/2.0\r\nVia: SIP/2.0/UDP 1.2.3.4:5060\r\n"
                          "Content-Type: Application/MANSCDP+xml\r\nContent-Length: 5\r\n"
                          "\r\nhello";
        gb_sip_msg bm;

        CHECK(gb_sip_parse(raw, strlen(raw), "1.2.3.4", 5060, &bm) == 0);
        CHECK(bm.body_len == 5);
        CHECK(memcmp(bm.body, "hello", 5) == 0);
    }
    {
        gb_sip_msg req;
        char resp[4096];
        int rn;
        gb_sip_msg rm;

        memset(&req, 0, sizeof(req));
        snprintf(req.method, sizeof(req.method), "MESSAGE");
        snprintf(req.uri, sizeof(req.uri), "sip:34020000001320000001@192.168.1.10:5060");
        strcpy(req.body, "<Query><CmdType>Catalog</CmdType><SN>1</SN></Query>");
        req.body_len = strlen(req.body);
        {
            gb_sip_header *h;
            h = &req.hdrs[req.hdr_count++];
            snprintf(h->name, sizeof(h->name), "Via");
            snprintf(h->value, sizeof(h->value), "SIP/2.0/UDP 192.168.1.100:5060;branch=z9hG4bK1");
            h = &req.hdrs[req.hdr_count++];
            snprintf(h->name, sizeof(h->name), "From");
            snprintf(h->value, sizeof(h->value),
                     "<sip:34020000002000000001@3402000000>;tag=xyz");
            h = &req.hdrs[req.hdr_count++];
            snprintf(h->name, sizeof(h->name), "To");
            snprintf(h->value, sizeof(h->value), "<sip:34020000001320000001@192.168.1.10:5060>");
            h = &req.hdrs[req.hdr_count++];
            snprintf(h->name, sizeof(h->name), "Call-ID");
            snprintf(h->value, sizeof(h->value), "call-2");
            h = &req.hdrs[req.hdr_count++];
            snprintf(h->name, sizeof(h->name), "CSeq");
            snprintf(h->value, sizeof(h->value), "1 MESSAGE");
        }
        rn = gb_sip_build_response(resp, sizeof(resp), &req, 200, "OK", "192.168.1.10", 5060,
                                   "devtag", "34020000001320000001", NULL, NULL, 0);
        CHECK(rn > 0);
        CHECK(gb_sip_parse(resp, (size_t)rn, "192.168.1.10", 5060, &rm) == 0);
        CHECK(rm.is_response);
        CHECK(rm.status == 200);
        CHECK(strstr(gb_sip_hdr(&rm, "To"), "tag=devtag") != NULL);
        CHECK(strcmp(gb_sip_hdr(&rm, "Call-ID"), "call-2") == 0);
    }
    return 0;
}

static int test_digest(void)
{
    gb_digest_challenge ch;
    char auth[512];
    char src[256];
    char ha1[33];
    char ha2[33];
    char kd[128];
    char expected[33];

    CHECK(gb_digest_parse_challenge(
              "Digest realm=\"3402000000\", nonce=\"abcdef\", algorithm=MD5", &ch) == 0);
    CHECK(strcmp(ch.realm, "3402000000") == 0);
    CHECK(strcmp(ch.nonce, "abcdef") == 0);

    CHECK(gb_digest_build_authorization("34020000001320000001", "12345678", "REGISTER",
                                        "sip:34020000002000000001@3402000000", &ch, 1, auth,
                                        sizeof(auth)) == 0);
    CHECK(strstr(auth, "username=\"34020000001320000001\"") != NULL);
    CHECK(strstr(auth, "nonce=\"abcdef\"") != NULL);

    snprintf(src, sizeof(src), "%s:%s:%s", "34020000001320000001", "3402000000", "12345678");
    zms_md5_hex_str(src, ha1);
    snprintf(src, sizeof(src), "%s:%s", "REGISTER",
             "sip:34020000002000000001@3402000000");
    zms_md5_hex_str(src, ha2);
    snprintf(kd, sizeof(kd), "%s:%s:%s", ha1, "abcdef", ha2);
    zms_md5_hex_str(kd, expected);
    CHECK(strstr(auth, expected) != NULL);

    /* qop=auth */
    CHECK(gb_digest_parse_challenge(
              "Digest realm=\"r\", nonce=\"n\", qop=\"auth\", algorithm=MD5", &ch) == 0);
    CHECK(gb_digest_build_authorization("u", "p", "REGISTER", "sip:r", &ch, 3, auth,
                                        sizeof(auth)) == 0);
    CHECK(strstr(auth, "qop=auth") != NULL);
    CHECK(strstr(auth, "nc=00000003") != NULL);
    return 0;
}

static int test_manscdp(void)
{
    gb_manscdp_msg m;
    char buf[2048];
    const char *query =
        "<?xml version=\"1.0\"?><Query><CmdType>Catalog</CmdType><SN>42</SN>"
        "<DeviceID>34020000001320000001</DeviceID></Query>";

    CHECK(gb_manscdp_parse(query, strlen(query), &m) == 0);
    CHECK(strcmp(m.root, "Query") == 0);
    CHECK(strcmp(m.cmd_type, "Catalog") == 0);
    CHECK(strcmp(m.sn, "42") == 0);
    CHECK(strcmp(m.device_id, "34020000001320000001") == 0);

    {
        gb_catalog_item items[1];
        memset(items, 0, sizeof(items));
        items[0].device_id = "34020000001320000001";
        items[0].name = "CAM 1 & 2";
        items[0].parent_id = "34020000001320000000";
        items[0].online = 1;
        CHECK(gb_manscdp_build_catalog(buf, sizeof(buf), "42", "34020000001320000000", items, 1) >
              0);
        CHECK(strstr(buf, "<CmdType>Catalog</CmdType>") != NULL);
        CHECK(strstr(buf, "<SN>42</SN>") != NULL);
        CHECK(strstr(buf, "<SumNum>1</SumNum>") != NULL);
        CHECK(strstr(buf, "CAM 1 &amp; 2") != NULL);
        CHECK(strstr(buf, "<Status>ON</Status>") != NULL);
    }
    {
        gb_record_item rec;
        memset(&rec, 0, sizeof(rec));
        rec.device_id = "ch1";
        rec.name = "20261109100000.mp4";
        rec.file_path = "cam1/2026-11-09/20261109100000.mp4";
        rec.start_time = "2026-11-09T10:00:00";
        rec.end_time = "2026-11-09T10:01:00";
        rec.type = "time";
        CHECK(gb_manscdp_build_record_info(buf, sizeof(buf), "9", "ch1", &rec, 1, 1) > 0);
        CHECK(strstr(buf, "<CmdType>RecordInfo</CmdType>") != NULL);
        CHECK(strstr(buf, "<StartTime>2026-11-09T10:00:00</StartTime>") != NULL);
    }
    return 0;
}

int main(void)
{
    if (test_sip_roundtrip() != 0 || test_digest() != 0 || test_manscdp() != 0) {
        return 1;
    }
    printf("test_gb28181_sip ok\n");
    return 0;
}
