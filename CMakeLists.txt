cmake_minimum_required(VERSION 3.14)
project(zms C)

if (WIN32)
    # 全局避免 windows.h 拉起 winsock.h 与各库显式引入的 winsock2.h 冲突
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
endif ()

# MSVC: source/execution charset UTF-8 (avoids C4819 when headers use UTF-8 comments)
if(MSVC)
    add_compile_options(/utf-8 /wd4819)
endif()

set(ZMS_VERSION_MAJOR 0)
set(ZMS_VERSION_MINOR 5)
set(ZMS_VERSION_PATCH 0)

option(ZN_BUILD_SHARED "Build shared znet library" OFF)
option(ZN_BUILD_TESTS "Legacy alias: enable CTest" OFF)
option(ZN_BUILD_EXAMPLES "Build ZMS demo executables" ON)
option(ZN_ENABLE_EPOLL "Use epoll poller backend (Linux)" ON)
option(ZN_ENABLE_WEPOLL "Use wepoll poller backend (Win32)" ON)
option(ZN_ENABLE_SELECT "Use select poller backend (generic POSIX/RTOS)" ON)
option(ZN_ALLOW_WIN32_STUB "Build Win32 stub poller (no wepoll) for IDE check" OFF)
option(ZN_ENABLE_OPENSSL "Enable OpenSSL TLS client (zn_tls_client)" ON)
option(ZMS_BUILD_SHARED "Build shared ZMS library" OFF)
option(ZMS_ENABLE_SRT "Build SRT transport support (requires libsrt)" OFF)
option(ZMS_BUILD_TESTS "Build ZMS automated tests (Stable SDK lifecycle)" OFF)

# Local overrides (paths, feature toggles) — run tools/configure.ps1 or configure.sh
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/build.local.cmake OPTIONAL)

if (ZN_BUILD_TESTS OR ZMS_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif ()

set(_ZMS_ZNET "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/znet/CMakeLists.txt")
set(_ZMS_KIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zmedia")
if (NOT EXISTS "${_ZMS_ZNET}")
    message(FATAL_ERROR
        "Missing vendored source 3rdpart/znet.\n"
        "This repository commits its dependencies in-tree; restore that directory.")
endif ()
if (NOT EXISTS "${_ZMS_KIT_ROOT}/librtp/CMakeLists.txt")
    message(FATAL_ERROR
        "Missing vendored source 3rdpart/zmedia.\n"
        "This repository commits its dependencies in-tree; restore that directory.")
endif ()

if(ZN_ENABLE_OPENSSL AND NOT OPENSSL_ROOT_DIR)
    if(WIN32 AND EXISTS "C:/Program Files/OpenSSL-Win64/include/openssl/ssl.h")
        set(OPENSSL_ROOT_DIR "C:/Program Files/OpenSSL-Win64" CACHE PATH "OpenSSL install prefix")
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zbase)

add_subdirectory(3rdpart/znet)

set(ZMS_KIT "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zmedia")
add_subdirectory(${ZMS_KIT}/libcore)
add_subdirectory(${ZMS_KIT}/libmpeg)
add_subdirectory(${ZMS_KIT}/libflv)
add_subdirectory(${ZMS_KIT}/libhls)
add_subdirectory(${ZMS_KIT}/librtp)
add_subdirectory(${ZMS_KIT}/libsdp)
add_subdirectory(${ZMS_KIT}/libmov)
add_subdirectory(${ZMS_KIT}/libmkv)
add_subdirectory(${ZMS_KIT}/libdash)

# Third-party kit: suppress vendor warnings so ZMS builds clean.
foreach(_zms_3p libflv libdash libhls libmkv libmpeg librtp libsdp libmov)
    if(TARGET ${_zms_3p})
        if(MSVC)
            target_compile_options(${_zms_3p} PRIVATE /wd4018 /wd4996 /wd4244)
        else()
            target_compile_options(${_zms_3p} PRIVATE -w)
        endif()
    endif()
endforeach()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/zms_sources.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/srt.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/webrtc.cmake)

# SRT service/session sources include <srt/srt.h>, so only add them when libsrt
# was actually found (streamid.c is portable and lives in ZMS_PROTOCOL_SOURCES).
if (ZMS_HAVE_SRT)
    list(APPEND ZMS_PROTOCOL_SOURCES
        src/protocol/srt/dispatch.c
        src/protocol/srt/poller.c
        src/protocol/srt/srt_server.c
        src/protocol/srt/session.c
        src/protocol/srt/session_play.c
        src/protocol/srt/session_publish.c)
endif ()


if(ZMS_ENABLE_WEBRTC AND ZMS_HAVE_ZICE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zice)
endif()

if (ZMS_ENABLE_WEBRTC)
    foreach(_src ${ZMS_WEBRTC_SOURCES})
        if(_src MATCHES "^src/(publish|play|vod|pull)/")
            list(APPEND ZMS_ROLE_SOURCES ${_src})
        else()
            list(APPEND ZMS_PROTOCOL_SOURCES ${_src})
        endif()
    endforeach()
    message(STATUS "ZMS: WebRTC WHEP enabled")
    if(ZMS_HAVE_WEBRTC_DTLS)
        message(STATUS "ZMS: WebRTC DTLS-SRTP enabled (OpenSSL)")
    else()
        message(WARNING "ZMS: WebRTC without OpenSSL — DTLS/SRTP disabled; browser play will not work")
    endif()
endif()

# ── Per-layer object libraries ─────────────────────────────────────────────
# The final `zms` archive aggregates all layers. Object libraries keep the
# build graph explicit without imposing a link order on cyclic callbacks.
set(ZMS_LAYER_DEFS "")
if (ZMS_HAVE_SRT)
    list(APPEND ZMS_LAYER_DEFS ZMS_HAVE_SRT=1)
endif ()
if (ZMS_ENABLE_WEBRTC)
    list(APPEND ZMS_LAYER_DEFS ZMS_ENABLE_WEBRTC=1)
endif ()
if (ZMS_HAVE_WEBRTC_DTLS)
    list(APPEND ZMS_LAYER_DEFS ZMS_HAVE_WEBRTC_DTLS=1)
endif ()
if (ZMS_HAVE_ZICE)
    list(APPEND ZMS_LAYER_DEFS ZMS_WEBRTC_USE_ZICE=1)
endif ()
if (WIN32 AND ZMS_ENABLE_WEBRTC AND ZMS_HAVE_ZICE)
    list(APPEND ZMS_LAYER_DEFS OS_WINDOWS=1)
endif ()
if (ZMS_BUILD_SHARED)
    list(APPEND ZMS_LAYER_DEFS ZMS_EXPORTS)
endif ()
list(APPEND ZMS_LAYER_DEFS
    "ZMS_VERSION_STRING=\"${ZMS_VERSION_MAJOR}.${ZMS_VERSION_MINOR}.${ZMS_VERSION_PATCH}\"")

function(zms_add_layer_target name)
    add_library(${name} OBJECT ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ZMS_KIT}/libmov/source)
    if (ZMS_ENABLE_WEBRTC AND ZMS_HAVE_ZICE)
        target_include_directories(${name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zice/src
            ${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zice/include)
    endif ()
    target_compile_definitions(${name} PRIVATE ${ZMS_LAYER_DEFS})
    target_link_libraries(${name} PRIVATE
        znet zbase libmpeg libflv libhls librtp libsdp libmov libmkv libdash zmedia_core)
    if (ZMS_ENABLE_WEBRTC AND ZMS_HAVE_ZICE)
        target_link_libraries(${name} PRIVATE zice)
    endif ()
    if (ZMS_HAVE_SRT)
        target_link_libraries(${name} PRIVATE ${ZMS_SRT_LIBS})
    endif ()
    if (MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif ()
    set_target_properties(${name} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
endfunction()

zms_add_layer_target(zms_core ${ZMS_CORE_SOURCES})
zms_add_layer_target(zms_media ${ZMS_MEDIA_SOURCES})
zms_add_layer_target(zms_bridge ${ZMS_BRIDGE_SOURCES})
zms_add_layer_target(zms_protocol ${ZMS_PROTOCOL_SOURCES})
zms_add_layer_target(zms_session ${ZMS_SESSION_SOURCES})
zms_add_layer_target(zms_roles ${ZMS_ROLE_SOURCES})
zms_add_layer_target(zms_egress ${ZMS_EGRESS_SOURCES})
zms_add_layer_target(zms_storage ${ZMS_STORAGE_SOURCES})
zms_add_layer_target(zms_ops ${ZMS_OPS_SOURCES})
zms_add_layer_target(zms_util ${ZMS_UTIL_SOURCES})

set(ZMS_OBJECTS
    $<TARGET_OBJECTS:zms_core>
    $<TARGET_OBJECTS:zms_media>
    $<TARGET_OBJECTS:zms_bridge>
    $<TARGET_OBJECTS:zms_protocol>
    $<TARGET_OBJECTS:zms_session>
    $<TARGET_OBJECTS:zms_roles>
    $<TARGET_OBJECTS:zms_egress>
    $<TARGET_OBJECTS:zms_storage>
    $<TARGET_OBJECTS:zms_ops>
    $<TARGET_OBJECTS:zms_util>)

if (ZMS_BUILD_SHARED)
    add_library(zms SHARED ${ZMS_OBJECTS})
    target_compile_definitions(zms PRIVATE ZMS_EXPORTS)
else ()
    add_library(zms STATIC ${ZMS_OBJECTS})
endif ()

target_include_directories(zms
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ZMS_KIT}/libmov/source
)

target_link_libraries(zms PUBLIC znet zbase libmpeg libflv libhls librtp libsdp libmov libmkv libdash)
# ZM shared model is used internally by src/bridge; keep it out of the public SDK surface.
target_link_libraries(zms PRIVATE zmedia_core)
if (ZMS_HAVE_SRT)
    target_compile_definitions(zms PUBLIC ZMS_HAVE_SRT=1)
    target_link_libraries(zms PRIVATE ${ZMS_SRT_LIBS})
endif ()
if (ZMS_ENABLE_WEBRTC)
    target_compile_definitions(zms PUBLIC ZMS_ENABLE_WEBRTC=1)
    if(ZMS_HAVE_WEBRTC_DTLS)
        target_compile_definitions(zms PUBLIC ZMS_HAVE_WEBRTC_DTLS=1)
    endif()
    if(ZMS_HAVE_ZICE)
        target_compile_definitions(zms PRIVATE ZMS_WEBRTC_USE_ZICE=1)
        target_include_directories(zms PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zice/src)
        if(WIN32)
            target_compile_definitions(zms PRIVATE OS_WINDOWS=1)
        endif()
        target_link_libraries(zms PRIVATE zice)
        if(UNIX)
            # zice 的 darray 等源码使用 sqrt，静态库不会自动带上 libm。
            target_link_libraries(zms PRIVATE m)
        endif()
    endif()
endif()
if(WIN32)
    target_link_libraries(zms PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_definitions(zms PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_compile_definitions(zms PUBLIC
    "ZMS_VERSION_STRING=\"${ZMS_VERSION_MAJOR}.${ZMS_VERSION_MINOR}.${ZMS_VERSION_PATCH}\"")

set_target_properties(zms PROPERTIES
    OUTPUT_NAME zms
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if (ZN_BUILD_EXAMPLES AND (ZN_HAVE_EPOLL OR ZN_HAVE_WEPOLL OR ZN_HAVE_SELECT))
    add_subdirectory(examples)
endif ()

if (ZMS_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

if (ZMS_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif ()

install(TARGETS zms
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/zms DESTINATION include
    FILES_MATCHING PATTERN "*.h")
