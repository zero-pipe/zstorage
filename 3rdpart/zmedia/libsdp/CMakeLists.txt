# libsdp: SDP syntax + attribute codecs + zm_sdp facade.
# No RTSP session state: methods, transport, interleaved framing and the
# RTP output muxer live in zero-media-server.

set(LIBSDP_ROOT "${CMAKE_CURRENT_LIST_DIR}")

set(LIBSDP_SOURCES
    "${LIBSDP_ROOT}/source/base64.c"
    "${LIBSDP_ROOT}/source/sdp.c"
    "${LIBSDP_ROOT}/source/sdp-a-rtpmap.c"
    "${LIBSDP_ROOT}/source/sdp-a-fmtp.c"
    "${LIBSDP_ROOT}/source/sdp-options.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-payload.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-h264.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-h265.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-h266.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-aac.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-mpeg2.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-mpeg4.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-g7xx.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-vpx.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-av1.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-opus.c"
    "${LIBSDP_ROOT}/source/sdp/sdp-fmtp-load.c"
    "${LIBSDP_ROOT}/src/zm_sdp.c"
)

add_library(libsdp STATIC ${LIBSDP_SOURCES})
target_include_directories(libsdp
    PUBLIC "${LIBSDP_ROOT}/include"
    PRIVATE "${LIBSDP_ROOT}/source"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../libflv/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../libmov/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../libmkv/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../libmpeg/include"
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../librtp/include"
)
target_compile_definitions(libsdp PRIVATE ZM_LIBSDP=1)

target_link_libraries(libsdp PUBLIC librtp libflv libmpeg libmov libmkv zmedia_core)

if (WIN32)
    target_compile_definitions(libsdp PRIVATE OS_WINDOWS=1)
endif ()
if (MSVC)
    target_compile_definitions(libsdp PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(libsdp PRIVATE /wd4267 /wd4101 /wd4828)
endif ()

set_target_properties(libsdp PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
