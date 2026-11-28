# zice: vendored under 3rdpart/zice (ireader/sdk subset).

set(_ZCS_ZICE_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zice")

if(NOT ZCS_ZICE_ROOT)
    set(ZCS_ZICE_ROOT "${_ZCS_ZICE_VENDOR}" CACHE PATH "Path to ireader zice tree (default: 3rdpart/zice)")
endif()

option(ZCS_WEBRTC_USE_ZICE "Use vendored zice for WebRTC ICE/STUN/TURN" ON)

if(ZCS_WEBRTC_USE_ZICE)
    if(NOT EXISTS "${ZCS_ZICE_ROOT}/include/zice/ice-agent.h")
        message(WARNING "ZCS_WEBRTC_USE_ZICE=ON but zice not found at ${ZCS_ZICE_ROOT}; WebRTC ICE disabled")
        set(ZCS_WEBRTC_USE_ZICE OFF CACHE BOOL "Use vendored zice for WebRTC ICE/STUN/TURN" FORCE)
    else()
        set(ZCS_HAVE_ZICE 1)
        message(STATUS "ZCS: WebRTC zice enabled (${ZCS_ZICE_ROOT})")
    endif()
endif()
