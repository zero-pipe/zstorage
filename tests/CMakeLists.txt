# ZCS automated tests. Enable with -DZCS_BUILD_TESTS=ON.
#
# Tests are plain C executables returning 0 on success, 1 on failure,
# registered with CTest. Keep them free of network ports and filesystem
# side effects unless a fixture directory is provided explicitly.

function(zcs_add_test name)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE zcs znet zmedia_core)
    if(MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

zcs_add_test(test_zbase)
zcs_add_test(test_zbase_os)
zcs_add_test(test_znet_tcp_shutdown)
zcs_add_test(test_znet_tcp_timeout)
zcs_add_test(test_byte_buf)
zcs_add_test(test_poller_timer)
zcs_add_test(test_auth_token)
zcs_add_test(test_http_parser)
zcs_add_test(test_sink_pull)
zcs_add_test(test_proto_msg)
zcs_add_test(test_amf_args)
zcs_add_test(test_gop_trim)
zcs_add_test(test_gop_clear_race)
zcs_add_test(test_dns_cache)
zcs_add_test(test_zstorage_slot)
zcs_add_test(test_zstorage_pool)
zcs_add_test(test_core_smoke)
zcs_add_test(test_zm_core)
zcs_add_test(test_bridge)
zcs_add_test(test_rtp_port)
zcs_add_test(test_rtmp_handshake)
zcs_add_test(test_zm_flv)
zcs_add_test(test_zm_flv_file)
target_compile_definitions(test_zm_flv_file PRIVATE
    ZCS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zcs_add_test(test_zm_rtp)
zcs_add_test(test_zm_mpeg)
zcs_add_test(test_zm_mov)
target_compile_definitions(test_zm_mov PRIVATE
    ZCS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zcs_add_test(test_zm_mkv)
target_compile_definitions(test_zm_mkv PRIVATE
    ZCS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zcs_add_test(test_zm_mov_write)
zcs_add_test(test_zm_ts_file)
target_compile_definitions(test_zm_ts_file PRIVATE
    ZCS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zcs_add_test(test_zm_sdp)
zcs_add_test(test_g711_mute_audio)
zcs_add_test(test_base64)
zcs_add_test(test_egress_clock)
zcs_add_test(test_egress_pacing)
zcs_add_test(test_http_request_reader)
zcs_add_test(test_config_ini)
target_compile_definitions(test_config_ini PRIVATE
    ZCS_CONFIG_INI="${PROJECT_SOURCE_DIR}/conf/config.ini")
# WebRTC 关闭时 srtp 符号不参与链接，跳过该测试目标。
if (ZCS_ENABLE_WEBRTC)
    zcs_add_test(test_webrtc_rtcp)
endif ()
zcs_add_test(test_zm_mkv_write)

# 错误码风格检查（docs/stream-pipeline.md 第 7.1 节）：
# zn_err_t 函数不得 return -1（-1 == ZN_ERR_NOMEM）。
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME check_errno_style
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/check_errno_style.py
                     ${PROJECT_SOURCE_DIR}/src)
endif()

# 端到端冒烟：起真实服务 → 推流 → FLV/HLS/WebAPI。
# 仅在有 ffmpeg 时执行推流部分；无 ffmpeg 时该部分 SKIP（不判失败）。
# 需要本机端口可用，故打 integration 标签：本地用
#   ctest -L integration        单独跑
#   ctest -LE integration       跳过（CI 单元测试作业默认跳过）
if(ZN_BUILD_EXAMPLES AND Python3_Interpreter_FOUND)
    add_test(NAME smoke_e2e
             COMMAND ${Python3_EXECUTABLE}
                     ${PROJECT_SOURCE_DIR}/tests/smoke_e2e.py)
    set_tests_properties(smoke_e2e PROPERTIES
        LABELS "integration"
        TIMEOUT 120)
endif()
