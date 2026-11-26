# 默认 ON：平台的「超低延迟」播放档依赖 WebRTC，默认关掉会让配置里的
# [webrtc] enable=1 变成空转（页面选中超低延迟后必然黑屏）。
# 依赖全部内置（3rdpart/zice + OpenSSL），无外部包；不需要时显式 -DZMS_ENABLE_WEBRTC=OFF。
option(ZMS_ENABLE_WEBRTC "Build WebRTC WHEP/WHIP (ICE-lite; DTLS when OpenSSL available)" ON)

if(ZMS_ENABLE_WEBRTC)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/zice.cmake)
    # 会话/业务层：src/session/webrtc（与 RTSP/RTMP/HTTP 一致的分层）
    list(APPEND ZMS_SESSION_SOURCES
        src/session/webrtc/webrtc_server.c
        src/session/webrtc/session.c
        src/session/webrtc/sdp.c
        src/session/webrtc/sdp_answer.c
        src/session/webrtc/sdp_client.c
        src/session/webrtc/whep_route.c
        src/session/webrtc/whip_route.c
        src/session/webrtc/dispatch.c
        src/session/webrtc/play_session.c
        src/session/webrtc/play_rtp.c
        src/session/webrtc/whip_ingress.c
        src/session/webrtc/signaling_client.c
        src/session/webrtc/client.c
        src/session/webrtc/client_http.c
    )
    # 协议层：仅编解码/传输原语，不依赖会话（ICE 经 ice_host.h 访问会话）
    set(ZMS_WEBRTC_SOURCES
        src/protocol/webrtc/stun.c
        src/protocol/webrtc/dtls.c
        src/media/wire/srtp.c
        src/protocol/webrtc/media_gateway.c
        src/protocol/webrtc/rtcp.c
        src/protocol/webrtc/rtp_twcc.c
        src/protocol/webrtc/ice.c
    )
    if(ZMS_HAVE_ZICE)
        list(APPEND ZMS_WEBRTC_SOURCES
            src/protocol/webrtc/ice_port.c
        )
    endif()
    if(ZN_HAVE_OPENSSL)
        set(ZMS_HAVE_WEBRTC_DTLS 1)
    endif()
endif()
