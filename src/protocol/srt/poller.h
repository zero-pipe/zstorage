/**
 * @file poller.h
 * @brief SRT poller 辅助。
 */
#ifndef ZMS_SRC_PROTOCOL_SRT_POLLER_H
#define ZMS_SRC_PROTOCOL_SRT_POLLER_H

#include "server_internal.h"

struct zms_srt_server;

void zms_srt_poller_init(struct zms_srt_server *srv);
void zms_srt_poller_fini(struct zms_srt_server *srv);

zms_srt_session *zms_srt_session_find(struct zms_srt_server *srv, SRTSOCKET sock);
void zms_srt_session_link(struct zms_srt_server *srv, zms_srt_session *sess);
void zms_srt_session_unlink(struct zms_srt_server *srv, zms_srt_session *sess);

zms_srt_session *zms_srt_session_accept(struct zms_srt_server *srv, SRTSOCKET client,
                                        const char *streamid);

#endif /* ZMS_SRC_PROTOCOL_SRT_POLLER_H */
