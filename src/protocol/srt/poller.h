/**
 * @file poller.h
 * @brief SRT poller 辅助。
 */
#ifndef ZCS_SRC_PROTOCOL_SRT_POLLER_H
#define ZCS_SRC_PROTOCOL_SRT_POLLER_H

#include "server_internal.h"

struct zcs_srt_server;

void zcs_srt_poller_init(struct zcs_srt_server *srv);
void zcs_srt_poller_fini(struct zcs_srt_server *srv);

zcs_srt_session *zcs_srt_session_find(struct zcs_srt_server *srv, SRTSOCKET sock);
void zcs_srt_session_link(struct zcs_srt_server *srv, zcs_srt_session *sess);
void zcs_srt_session_unlink(struct zcs_srt_server *srv, zcs_srt_session *sess);

zcs_srt_session *zcs_srt_session_accept(struct zcs_srt_server *srv, SRTSOCKET client,
                                        const char *streamid);

#endif /* ZCS_SRC_PROTOCOL_SRT_POLLER_H */
