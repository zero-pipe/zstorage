# SRT optional transport (libsrt). Included from root CMakeLists.txt.

# Windows: copy srt.dll next to demo exe after build (when ZCS_SRT_ROOT/bin/srt.dll exists).
function(zcs_copy_srt_dll target)
    if (NOT WIN32 OR NOT ZCS_SRT_DLL OR NOT EXISTS "${ZCS_SRT_DLL}")
        return()
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZCS_SRT_DLL}" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copy srt.dll for ${target}"
        VERBATIM)
endfunction()

if (NOT ZCS_ENABLE_SRT)
    return()
endif ()

set(_ZCS_SRT_FOUND 0)

# Explicit prefix from tools/configure (mainly Windows).
if (ZCS_SRT_ROOT)
    file(TO_CMAKE_PATH "${ZCS_SRT_ROOT}" _zcs_srt_root)
    find_path(SRT_INCLUDE_DIR NAMES srt/srt.h PATHS "${_zcs_srt_root}/include" NO_DEFAULT_PATH)
    find_library(SRT_LIBRARY NAMES srt libsrt PATHS "${_zcs_srt_root}/lib" NO_DEFAULT_PATH)
    if (SRT_INCLUDE_DIR AND SRT_LIBRARY)
        set(_ZCS_SRT_FOUND 1)
        if (NOT TARGET zcs_srt_imported)
            add_library(zcs_srt_imported UNKNOWN IMPORTED)
        endif ()
        set_target_properties(zcs_srt_imported PROPERTIES
            IMPORTED_LOCATION "${SRT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SRT_INCLUDE_DIR}")
        message(STATUS "ZCS: SRT from ZCS_SRT_ROOT=${_zcs_srt_root}")
    else ()
        message(WARNING "ZCS_SRT_ROOT=${_zcs_srt_root} but srt/srt.h or libsrt not found under include/ and lib/")
    endif ()
endif ()

if (NOT _ZCS_SRT_FOUND)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(SRT QUIET IMPORTED_TARGET srt)
        if (SRT_FOUND)
            set(_ZCS_SRT_FOUND 1)
        endif ()
    endif ()
endif ()

if (NOT _ZCS_SRT_FOUND)
    find_path(SRT_INCLUDE_DIR NAMES srt/srt.h)
    find_library(SRT_LIBRARY NAMES srt libsrt)
    if (SRT_INCLUDE_DIR AND SRT_LIBRARY)
        set(_ZCS_SRT_FOUND 1)
        if (NOT TARGET zcs_srt_imported)
            add_library(zcs_srt_imported UNKNOWN IMPORTED)
        endif ()
        set_target_properties(zcs_srt_imported PROPERTIES
            IMPORTED_LOCATION "${SRT_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${SRT_INCLUDE_DIR}")
    endif ()
endif ()

if (NOT _ZCS_SRT_FOUND)
    message(WARNING "ZCS_ENABLE_SRT=ON but libsrt was not found (ZCS_SRT_ROOT, pkg-config srt, or srt/srt.h + libsrt)")
    return()
endif ()

set(ZCS_HAVE_SRT 1)

if (TARGET PkgConfig::SRT)
    set(ZCS_SRT_LIBS PkgConfig::SRT)
else ()
    set(ZCS_SRT_LIBS zcs_srt_imported)
endif ()

if (ZCS_SRT_ROOT)
    file(TO_CMAKE_PATH "${ZCS_SRT_ROOT}" _zcs_srt_root)
    set(ZCS_SRT_DLL "${_zcs_srt_root}/bin/srt.dll" CACHE INTERNAL "SRT runtime DLL for post-build copy")
endif ()

message(STATUS "ZCS: SRT transport enabled (libsrt found)")
