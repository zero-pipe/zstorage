/**
 * @file protocol_chunk_read.c
 * @brief RTMP chunk 读取与重组：按 csid 维护 chunk stream 状态。
 */
#include "protocol/rtmp/protocol_internal.h"
#include "zcs/util/buf_pool.h"
#include "zn/util/log.h"
#include <stdlib.h>
#include <string.h>

static zcs_rtmp_cs_state *get_cs(zcs_rtmp_protocol *p, uint32_t csid)
{
    zcs_rtmp_cs_state *c;

    if (csid >= ZCS_RTMP_MAX_CS) {
        return NULL;
    }
    c = p->cs[csid];
    if (c) {
        return c;
    }
    c = (zcs_rtmp_cs_state *)calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->csid = csid;
    p->cs[csid] = c;
    return c;
}

void rtmp_chunk_cs_reset_all(zcs_rtmp_protocol *p)
{
    int i;

    if (!p) {
        return;
    }
    for (i = 0; i < (int)ZCS_RTMP_MAX_CS; ++i) {
        if (p->cs[i]) {
            p->cs[i]->msg_len = 0;
            p->cs[i]->body_off = 0;
        }
    }
}

static zn_err_t ensure_body(zcs_rtmp_protocol *p, zcs_rtmp_cs_state *c, size_t need)
{
    size_t cap;

    if (c->body_cap >= need) {
        return ZN_OK;
    }
    cap = need < 256 ? 256 : need;
    if (p && p->io_poller) {
        if (!zcs_buf_pool_slot_resize_poller(&c->body, &c->body_cap, cap, p->io_poller)) {
            return ZN_ERR_NOMEM;
        }
    } else if (!zcs_buf_pool_slot_resize(&c->body, &c->body_cap, cap)) {
        return ZN_ERR_NOMEM;
    }
    return ZN_OK;
}

static void dispatch_msg(zcs_rtmp_protocol *p, zcs_rtmp_cs_state *c)
{
    if (c->body_off < c->msg_len) {
        return;
    }

    if (c->type_id == ZCS_RTMP_MSG_VIDEO || c->type_id == ZCS_RTMP_MSG_AUDIO) {
        static unsigned disp_cnt;
        if (disp_cnt < 100) {
            zn_info("RTMP dispatch csid=%u type=%u len=%u tag_dts_ms=%u", (unsigned)c->csid,
                     (unsigned)c->type_id, (unsigned)c->msg_len, (unsigned)c->tag_dts_ms);
        }
        ++disp_cnt;
    }

    if (c->type_id == ZCS_RTMP_MSG_SET_CHUNK && c->msg_len >= 4) {
        p->in_chunk_size = rtmp_read_be32(c->body);
        if (p->in_chunk_size == 0) {
            p->in_chunk_size = ZCS_RTMP_DEFAULT_CHUNK;
        }
        zn_info("RTMP peer SetChunkSize=%u", (unsigned)p->in_chunk_size);
    }

    zcs_rtmp_chunk chunk = {
        .type_id = c->type_id,
        .stream_id = c->stream_id,
        .tag_dts_ms = c->tag_dts_ms,
        .body = c->body,
        .body_size = c->msg_len,
    };
    if (c->type_id == ZCS_RTMP_MSG_CMD && p->opts.on_cmd && c->body && c->msg_len > 0) {
        zcs_amf_value *cmd = NULL;
        size_t off = 0;
        if (zcs_amf_decode(c->body, c->msg_len, &cmd, &off) == ZN_OK && cmd &&
            zcs_amf_value_type(cmd) == ZCS_AMF_STRING) {
            static unsigned cmd_cnt;
            if (cmd_cnt < 20) {
                zn_info("RTMP cmd csid=%u len=%u: %s", (unsigned)c->csid, (unsigned)c->msg_len,
                         zcs_amf_value_str(cmd));
            }
            ++cmd_cnt;
            double trans = 0;
            if (off < c->msg_len) {
                zcs_amf_value *tid = NULL;
                size_t tid_len = 0;
                if (zcs_amf_decode(c->body + off, c->msg_len - off, &tid, &tid_len) == ZN_OK &&
                    tid) {
                    if (zcs_amf_value_type(tid) == ZCS_AMF_NUMBER) {
                        trans = zcs_amf_value_num(tid);
                    }
                    zcs_amf_value_free(tid);
                    off += tid_len;
                }
            }
            p->opts.on_cmd(zcs_amf_value_str(cmd), trans, c->body + off, c->msg_len - off,
                           p->opts.user);
        }
        zcs_amf_value_free(cmd);
    } else if (p->opts.on_chunk) {
        p->opts.on_chunk(&chunk, p->opts.user);
    }
    c->last_msg_len = c->msg_len;
    c->last_type_id = c->type_id;
    c->last_stream_id = c->stream_id;
    c->last_tag_dts_ms = c->tag_dts_ms;
    c->last_tag_dts_field = c->tag_dts_field;
    c->body_off = 0;
    c->msg_len = 0;
}

static void cs_restore_from_last(zcs_rtmp_cs_state *c)
{
    if (c->last_msg_len == 0) {
        return;
    }
    c->msg_len = c->last_msg_len;
    c->type_id = c->last_type_id;
    c->stream_id = c->last_stream_id;
    c->tag_dts_ms = c->last_tag_dts_ms;
    c->tag_dts_field = c->last_tag_dts_field;
    c->body_off = 0;
}

zn_err_t rtmp_chunk_parse(zcs_rtmp_protocol *p, const uint8_t *data, size_t len, size_t *consumed)
{
    static const size_t HEADER_LENGTH[] = {12, 8, 4, 1};
    const uint8_t *ptr = data;
    const uint8_t *end = data + len;

    while (ptr < end) {
        if (ptr + 1 > end) {
            break;
        }

        uint8_t b0 = *ptr;
        uint8_t fmt = b0 >> 6;
        uint32_t csid = b0 & 0x3f;
        size_t offset = 0;

        if (csid == 0) {
            if (ptr + 2 > end) {
                break;
            }
            csid = 64 + (uint32_t)ptr[1];
            offset = 1;
        } else if (csid == 1) {
            if (ptr + 3 > end) {
                break;
            }
            csid = 64 + (uint32_t)ptr[2] + ((uint32_t)ptr[1] << 8);
            offset = 2;
        }

        if (fmt > 3) {
            break;
        }

        size_t header_len = HEADER_LENGTH[fmt];
        if ((size_t)(end - ptr) < header_len + offset) {
            break;
        }

        zcs_rtmp_cs_state *c = get_cs(p, csid);
        if (!c) {
            break;
        }
        c->csid = csid;

        if (c->body_off == 0 && c->msg_len == 0) {
            cs_restore_from_last(c);
            c->is_abs_stamp = 0;
        }

        const uint8_t *h = ptr + offset;
        switch (header_len) {
        case 12:
            c->is_abs_stamp = 1;
            c->stream_id = rtmp_read_le32(h + 8);
            /* fallthrough */
        case 8:
            c->msg_len = rtmp_read_be24(h + 4);
            c->type_id = h[7];
            c->body_off = 0;
            /* fallthrough */
        case 4:
            c->tag_dts_field = rtmp_read_be24(h + 1);
            break;
        default:
            break;
        }

        uint32_t header_tag_dts = c->tag_dts_field;
        if (c->tag_dts_field == 0xffffff) {
            if ((size_t)(end - ptr) < header_len + offset + 4) {
                break;
            }
            header_tag_dts = rtmp_read_be32(ptr + offset + header_len);
            offset += 4;
        }

        if (c->msg_len < c->body_off) {
            return ZN_ERR_INVALID;
        }

        size_t more = c->msg_len - c->body_off;
        if (more > p->in_chunk_size) {
            more = p->in_chunk_size;
        }

        if ((size_t)(end - ptr) < header_len + offset + more) {
            break;
        }

        if (more > 0) {
            if (ensure_body(p, c, c->body_off + more) != ZN_OK) {
                return ZN_ERR_NOMEM;
            }
            memcpy(c->body + c->body_off, ptr + offset + header_len, more);
            c->body_off += more;
        }

        ptr += offset + header_len + more;

        if (c->body_off >= c->msg_len) {
            c->tag_dts_ms = header_tag_dts + (c->is_abs_stamp ? 0 : c->tag_dts_ms);
            dispatch_msg(p, c);
        }
    }

    if (consumed) {
        *consumed = (size_t)(ptr - data);
    }
    return ZN_OK;
}
