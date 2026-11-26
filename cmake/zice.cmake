# zice: vendored under 3rdpart/zice (ireader/sdk subset).

set(_ZMS_ZICE_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zice")

if(NOT ZMS_ZICE_ROOT)
    set(ZMS_ZICE_ROOT "${_ZMS_ZICE_VENDOR}" CACHE PATH "Path to ireader zice tree (default: 3rdpart/zice)")
endif()

option(ZMS_WEBRTC_USE_ZICE "Use vendored zice for WebRTC ICE/STUN/TURN" ON)

if(ZMS_WEBRTC_USE_ZICE)
    if(NOT EXISTS "${ZMS_ZICE_ROOT}/include/zice/ice-agent.h")
        message(WARNING "ZMS_WEBRTC_USE_ZICE=ON but zice not found at ${ZMS_ZICE_ROOT}; WebRTC ICE disabled")
        set(ZMS_WEBRTC_USE_ZICE OFF CACHE BOOL "Use vendored zice for WebRTC ICE/STUN/TURN" FORCE)
    else()
        set(ZMS_HAVE_ZICE 1)
        message(STATUS "ZMS: WebRTC zice enabled (${ZMS_ZICE_ROOT})")
    endif()
endif()
