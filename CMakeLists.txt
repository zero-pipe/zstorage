cmake_minimum_required(VERSION 3.14)
project(zcs C)

if (WIN32)
    # 全局避免 windows.h 拉起 winsock.h 与各库显式引入的 winsock2.h 冲突
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
endif ()

# MSVC: source/execution charset UTF-8 (avoids C4819 when headers use UTF-8 comments)
if(MSVC)
    add_compile_options(/utf-8 /wd4819)
endif()

set(ZCS_VERSION_MAJOR 0)
set(ZCS_VERSION_MINOR 5)
set(ZCS_VERSION_PATCH 0)

option(ZN_BUILD_SHARED "Build shared znet library" OFF)
option(ZN_BUILD_TESTS "Legacy alias: enable CTest" OFF)
option(ZN_BUILD_EXAMPLES "Build ZCS demo executables" ON)
option(ZN_ENABLE_EPOLL "Use epoll poller backend (Linux)" ON)
option(ZN_ENABLE_WEPOLL "Use wepoll poller backend (Win32)" ON)
option(ZN_ENABLE_SELECT "Use select poller backend (generic POSIX/RTOS)" ON)
option(ZN_ALLOW_WIN32_STUB "Build Win32 stub poller (no wepoll) for IDE check" OFF)
option(ZN_ENABLE_OPENSSL "Enable OpenSSL TLS client (zn_tls_client)" ON)
option(ZCS_BUILD_SHARED "Build shared ZCS library" OFF)
option(ZCS_BUILD_TESTS "Build ZCS automated tests (Stable SDK lifecycle)" OFF)

# Local overrides (paths, feature toggles) — run tools/configure.ps1 or configure.sh
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/build.local.cmake OPTIONAL)

if (ZN_BUILD_TESTS OR ZCS_BUILD_TESTS)
    include(CTest)
    enable_testing()
endif ()

set(_ZCS_ZNET "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/znet/CMakeLists.txt")
set(_ZCS_KIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zmedia")
if (NOT EXISTS "${_ZCS_ZNET}")
    message(FATAL_ERROR
        "Missing vendored source 3rdpart/znet.\n"
        "This repository commits its dependencies in-tree; restore that directory.")
endif ()
if (NOT EXISTS "${_ZCS_KIT_ROOT}/librtp/CMakeLists.txt")
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

set(ZCS_KIT "${CMAKE_CURRENT_SOURCE_DIR}/3rdpart/zmedia")
add_subdirectory(${ZCS_KIT}/libcore)
add_subdirectory(${ZCS_KIT}/libmpeg)
add_subdirectory(${ZCS_KIT}/libflv)
add_subdirectory(${ZCS_KIT}/libhls)
add_subdirectory(${ZCS_KIT}/librtp)
add_subdirectory(${ZCS_KIT}/libsdp)
add_subdirectory(${ZCS_KIT}/libmov)
add_subdirectory(${ZCS_KIT}/libmkv)
add_subdirectory(${ZCS_KIT}/libdash)

# Third-party kit: suppress vendor warnings so ZCS builds clean.
foreach(_zcs_3p libflv libdash libhls libmkv libmpeg librtp libsdp libmov)
    if(TARGET ${_zcs_3p})
        if(MSVC)
            target_compile_options(${_zcs_3p} PRIVATE /wd4018 /wd4996 /wd4244)
        else()
            target_compile_options(${_zcs_3p} PRIVATE -w)
        endif()
    endif()
endforeach()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/zcs_sources.cmake)

# ── Per-layer object libraries ─────────────────────────────────────────────
# The final `zcs` archive aggregates all layers. Object libraries keep the
# build graph explicit without imposing a link order on cyclic callbacks.
set(ZCS_LAYER_DEFS "")
if (ZCS_BUILD_SHARED)
    list(APPEND ZCS_LAYER_DEFS ZCS_EXPORTS)
endif ()
list(APPEND ZCS_LAYER_DEFS
    "ZCS_VERSION_STRING=\"${ZCS_VERSION_MAJOR}.${ZCS_VERSION_MINOR}.${ZCS_VERSION_PATCH}\"")

function(zcs_add_layer_target name)
    add_library(${name} OBJECT ${ARGN})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ZCS_KIT}/libmov/source)
    target_compile_definitions(${name} PRIVATE ${ZCS_LAYER_DEFS})
    target_link_libraries(${name} PRIVATE
        znet zbase libmpeg libflv libhls librtp libsdp libmov libmkv libdash zmedia_core)
    if (MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif ()
    set_target_properties(${name} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        POSITION_INDEPENDENT_CODE ON)
endfunction()

zcs_add_layer_target(zcs_core ${ZCS_CORE_SOURCES})
zcs_add_layer_target(zcs_media ${ZCS_MEDIA_SOURCES})
zcs_add_layer_target(zcs_bridge ${ZCS_BRIDGE_SOURCES})
zcs_add_layer_target(zcs_protocol ${ZCS_PROTOCOL_SOURCES})
zcs_add_layer_target(zcs_session ${ZCS_SESSION_SOURCES})
zcs_add_layer_target(zcs_roles ${ZCS_ROLE_SOURCES})
zcs_add_layer_target(zcs_egress ${ZCS_EGRESS_SOURCES})
zcs_add_layer_target(zcs_storage ${ZCS_STORAGE_SOURCES})
zcs_add_layer_target(zcs_ops ${ZCS_OPS_SOURCES})
zcs_add_layer_target(zcs_util ${ZCS_UTIL_SOURCES})

set(ZCS_OBJECTS
    $<TARGET_OBJECTS:zcs_core>
    $<TARGET_OBJECTS:zcs_media>
    $<TARGET_OBJECTS:zcs_bridge>
    $<TARGET_OBJECTS:zcs_protocol>
    $<TARGET_OBJECTS:zcs_session>
    $<TARGET_OBJECTS:zcs_roles>
    $<TARGET_OBJECTS:zcs_egress>
    $<TARGET_OBJECTS:zcs_storage>
    $<TARGET_OBJECTS:zcs_ops>
    $<TARGET_OBJECTS:zcs_util>)

if (ZCS_BUILD_SHARED)
    add_library(zcs SHARED ${ZCS_OBJECTS})
    target_compile_definitions(zcs PRIVATE ZCS_EXPORTS)
else ()
    add_library(zcs STATIC ${ZCS_OBJECTS})
endif ()

target_include_directories(zcs
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ZCS_KIT}/libmov/source
)

target_link_libraries(zcs PUBLIC znet zbase libmpeg libflv libhls librtp libsdp libmov libmkv libdash)
# ZM shared model is used internally by src/bridge; keep it out of the public SDK surface.
target_link_libraries(zcs PRIVATE zmedia_core)
if(WIN32)
    target_link_libraries(zcs PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_definitions(zcs PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

target_compile_definitions(zcs PUBLIC
    "ZCS_VERSION_STRING=\"${ZCS_VERSION_MAJOR}.${ZCS_VERSION_MINOR}.${ZCS_VERSION_PATCH}\"")

set_target_properties(zcs PROPERTIES
    OUTPUT_NAME zcs
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

if (ZN_BUILD_EXAMPLES AND (ZN_HAVE_EPOLL OR ZN_HAVE_WEPOLL OR ZN_HAVE_SELECT))
    add_subdirectory(examples)
endif ()

if (ZCS_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

if (ZCS_BUILD_FUZZ)
    add_subdirectory(fuzz)
endif ()

install(TARGETS zcs
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY include/zcs DESTINATION include
    FILES_MATCHING PATTERN "*.h")
