# Vendored zice (ireader/sdk): ICE/STUN/TURN for ZMS WebRTC.
# Layout: include/zice/ (public ICE headers), src/ (impl). Timers use ZN via webrtc_ice_port.c.

set(ZICE_ROOT "${CMAKE_CURRENT_LIST_DIR}")

set(ZICE_SOURCES
    ${ZICE_ROOT}/src/ice-agent.c
    ${ZICE_ROOT}/src/ice-candidate.c
    ${ZICE_ROOT}/src/ice-checklist.c
    ${ZICE_ROOT}/src/ice-gather.c
    ${ZICE_ROOT}/src/ice-internal.c
    ${ZICE_ROOT}/src/ice-stream.c
    ${ZICE_ROOT}/src/stun-agent.c
    ${ZICE_ROOT}/src/stun-attr.c
    ${ZICE_ROOT}/src/stun-auth.c
    ${ZICE_ROOT}/src/stun-client.c
    ${ZICE_ROOT}/src/stun-message.c
    ${ZICE_ROOT}/src/stun-request.c
    ${ZICE_ROOT}/src/stun-response.c
    ${ZICE_ROOT}/src/stun-server.c
    ${ZICE_ROOT}/src/stun-transid.c
    ${ZICE_ROOT}/src/turn-agent.c
    ${ZICE_ROOT}/src/turn-allocate.c
    ${ZICE_ROOT}/src/turn-client.c
    ${ZICE_ROOT}/src/turn-server.c
)

if (NOT TARGET zbase)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../zbase" "${CMAKE_CURRENT_BINARY_DIR}/zbase")
endif()

add_library(zice STATIC ${ZICE_SOURCES})
target_link_libraries(zice PUBLIC zbase)
target_include_directories(zice
    PUBLIC
        ${ZICE_ROOT}/include
)
target_compile_definitions(zice PRIVATE ZICE_BUILD=1)

if(WIN32)
    target_compile_definitions(zice PRIVATE OS_WINDOWS=1)
endif()

if(MSVC)
    set(_zms_zice_forced "${ZICE_ROOT}/../../cmake/zice_forced.h")
    target_compile_options(zice PRIVATE /wd4018 /wd4996 /wd4244)
    target_compile_options(zice PRIVATE "/FI${_zms_zice_forced}")
else()
    target_compile_options(zice PRIVATE -w)
endif()

set_target_properties(zice PROPERTIES
    OUTPUT_NAME zice
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
