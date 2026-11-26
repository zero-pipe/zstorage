/**
 * @file poller.c
 * @brief SRT epoll 挂在 poller 定时器上：收包 → demux → ingest（poller 线程，PR-SRT-3）。
 */
#include "poller.h"
#include "zn/platform.h"
#include "zn/util/log.h"
#include "zn/util/timer.h"

#define ZMS_SRT_EPOLL_MAX 64
#define ZMS_SRT_POLL_MS 5
#define ZMS_SRT_STALL_TICK 500

static void srt_sock_nonblock(SRTSOCKET sock)
{
    int no = 0;

    if (sock == SRT_INVALID_SOCK) {
        return;
    }
    (void)srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof(no));
}

static int srt_read_streamid(SRTSOCKET sock, char *out, size_t out_cap)
{
    int len = (int)out_cap;

    out[0] = '\0';
    if (srt_getsockopt(sock, 0, SRTO_STREAMID, out, &len) != 0) {
        return -1;
    }
    if (len < 0 || (size_t)len >= out_cap) {
        return -1;
    }
    out[len] = '\0';
    return 0;
}

static void srt_service_poll_sessions(zms_srt_server *srv)
{
    zms_srt_session *s;

    if (!srv) {
        return;
    }
    for (s = srv->sessions; s; s = s->next) {
        if (s->stopping || s->destroy_scheduled) {
            continue;
        }
        if (s->mode == ZMS_SRT_SESSION_MODE_PUBLISH) {
            zms_srt_session_drain_recv(s);
            if (s->logged_recv && !s->logged_recv_stall && s->recv_bytes > 0 &&
                s->recv_bytes < 65536) {
                if (s->recv_bytes == s->last_recv_bytes) {
                    s->stall_polls++;
                } else {
                    s->last_recv_bytes = s->recv_bytes;
                    s->stall_polls = 0;
                }
                if (s->stall_polls >= ZMS_SRT_STALL_TICK) {
                    s->logged_recv_stall = 1;
                    zn_warn("SRT #%d recv stalled at %llu bytes (need ~750B+ for H264 SPS); use "
                             "latency=1200000&repeat-headers=1",
                             s->session_no, (unsigned long long)s->recv_bytes);
                }
            }
        } else {
            zms_srt_session_pump_send(s);
        }
    }
}

static void srt_service_on_listen(zms_srt_server *srv)
{
    struct sockaddr_storage addr;
    int addrlen = (int)sizeof(addr);
    SRTSOCKET client;
    char streamid[512];
    zms_srt_session *sess;

    client = srt_accept(srv->listen_sock, (struct sockaddr *)&addr, &addrlen);
    if (client == SRT_INVALID_SOCK) {
        return;
    }

    streamid[0] = '\0';
    (void)srt_read_streamid(client, streamid, sizeof(streamid));
    zn_info("SRT accept sock=%d streamid=%s", (int)client, streamid[0] ? streamid : "(empty)");

    sess = zms_srt_session_accept(srv, client, streamid[0] ? streamid : NULL);
    if (!sess) {
        zn_warn("SRT reject client sock=%d", (int)client);
        srt_close(client);
    }
}

static void srt_poller_tick(void *user)
{
    zms_srt_server *srv = (zms_srt_server *)user;
    SRTSOCKET ready[ZMS_SRT_EPOLL_MAX];
    int ready_len = ZMS_SRT_EPOLL_MAX;
    int i;

    if (!srv || !srv->running) {
        return;
    }

    if (srt_epoll_wait(srv->epoll_id, ready, &ready_len, NULL, NULL, 0, NULL, NULL, NULL, NULL) ==
        SRT_ERROR) {
        if (srt_getlasterror(NULL) != SRT_ETIMEOUT) {
            zn_warn("SRT epoll_wait: %s", srt_getlasterror_str());
        }
    } else {
        for (i = 0; i < ready_len; ++i) {
            SRTSOCKET sock = ready[i];
            SRT_SOCKSTATUS st = srt_getsockstate(sock);

            if (st == SRTS_BROKEN || st == SRTS_NONEXIST || st == SRTS_CLOSED) {
                if (sock == srv->listen_sock) {
                    continue;
                }
                zms_srt_session *sess = zms_srt_session_find(srv, sock);
                if (sess) {
                    zms_srt_session_schedule_destroy(sess);
                } else {
                    srt_close(sock);
                }
                continue;
            }
            if (sock == srv->listen_sock) {
                srt_service_on_listen(srv);
            } else {
                zms_srt_session *sess = zms_srt_session_find(srv, sock);
                if (sess) {
                    if (sess->mode == ZMS_SRT_SESSION_MODE_PUBLISH) {
                        zms_srt_session_drain_recv(sess);
                    } else {
                        zms_srt_session_pump_send(sess);
                    }
                }
            }
        }
    }
    srt_service_poll_sessions(srv);
}

void zms_srt_poller_init(zms_srt_server *srv)
{
    int events = SRT_EPOLL_IN | SRT_EPOLL_ERR;

    if (!srv || !srv->poller || srv->listen_sock == SRT_INVALID_SOCK) {
        return;
    }

    srv->epoll_id = srt_epoll_create();
    if (srv->epoll_id < 0) {
        return;
    }

    srt_sock_nonblock(srv->listen_sock);
    if (srt_epoll_add_usock(srv->epoll_id, srv->listen_sock, &events) == SRT_ERROR) {
        srt_epoll_release(srv->epoll_id);
        srv->epoll_id = -1;
        return;
    }

    srv->io_timer = zn_timer_start(srv->poller, ZMS_SRT_POLL_MS, 1, srt_poller_tick, srv);
    if (!srv->io_timer) {
        srt_epoll_release(srv->epoll_id);
        srv->epoll_id = -1;
    }
}

void zms_srt_poller_fini(zms_srt_server *srv)
{
    zms_srt_session *s;
    zms_srt_session *next;

    if (!srv) {
        return;
    }

    if (srv->io_timer) {
        zn_timer_stop(srv->io_timer);
        srv->io_timer = NULL;
    }

    for (s = srv->sessions; s; s = next) {
        next = s->next;
        zms_srt_session_destroy_now(s);
    }
    srv->sessions = NULL;

    if (srv->epoll_id >= 0) {
        srt_epoll_release(srv->epoll_id);
        srv->epoll_id = -1;
    }
}
