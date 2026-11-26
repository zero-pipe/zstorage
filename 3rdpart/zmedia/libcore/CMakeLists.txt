# ZM 共用媒体模型（packet / mem / track / codec registry）。
# 零依赖：不 link znet，也不 link 任何容器库。

set(LIBCORE_ROOT "${CMAKE_CURRENT_LIST_DIR}")

set(LIBCORE_SOURCES
    ${LIBCORE_ROOT}/src/mem.c
    ${LIBCORE_ROOT}/src/packet.c
    ${LIBCORE_ROOT}/src/track.c
    ${LIBCORE_ROOT}/src/codec.c
    ${LIBCORE_ROOT}/src/reader.c
    ${LIBCORE_ROOT}/src/writer.c
    ${LIBCORE_ROOT}/src/packager.c
)

add_library(zmedia_core STATIC ${LIBCORE_SOURCES})
target_include_directories(zmedia_core PUBLIC "${LIBCORE_ROOT}/include")
target_compile_definitions(zmedia_core PRIVATE ZM_LIBCORE=1)

if (MSVC)
    target_compile_definitions(zmedia_core PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(zmedia_core PRIVATE /wd4267 /wd4101 /wd4828 /wd4996)
endif ()

set_target_properties(zmedia_core PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
