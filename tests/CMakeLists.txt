# ZMS automated tests. Enable with -DZMS_BUILD_TESTS=ON.
#
# Tests are plain C executables returning 0 on success, 1 on failure,
# registered with CTest. Keep them free of network ports and filesystem
# side effects unless a fixture directory is provided explicitly.

function(zms_add_test name)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${name} PRIVATE zms znet zmedia_core)
    if(MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

zms_add_test(test_zbase)
zms_add_test(test_zbase_os)
zms_add_test(test_znet_tcp_shutdown)
zms_add_test(test_znet_tcp_timeout)
zms_add_test(test_mp4_retention)
zms_add_test(test_byte_buf)
zms_add_test(test_poller_timer)
zms_add_test(test_auth_token)
zms_add_test(test_http_parser)
zms_add_test(test_sink_pull)
zms_add_test(test_proto_msg)
zms_add_test(test_proto_xml)
zms_add_test(test_amf_args)
zms_add_test(test_gop_trim)
zms_add_test(test_gop_clear_race)
zms_add_test(test_dns_cache)
zms_add_test(test_mp4_recorder_async)
zms_add_test(test_zstorage_slot)
zms_add_test(test_zstorage_pool)
zms_add_test(test_core_smoke)
zms_add_test(test_zm_core)
zms_add_test(test_bridge)
zms_add_test(test_vod_file_demux)
target_compile_definitions(test_vod_file_demux PRIVATE
    ZMS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zms_add_test(test_rtp_port)
zms_add_test(test_rtmp_handshake)
zms_add_test(test_zm_flv)
zms_add_test(test_zm_flv_file)
target_compile_definitions(test_zm_flv_file PRIVATE
    ZMS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zms_add_test(test_zm_rtp)
zms_add_test(test_zm_mpeg)
zms_add_test(test_zm_mov)
target_compile_definitions(test_zm_mov PRIVATE
    ZMS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zms_add_test(test_zm_mkv)
target_compile_definitions(test_zm_mkv PRIVATE
    ZMS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zms_add_test(test_zm_mov_write)
zms_add_test(test_zm_hls)
zms_add_test(test_zm_hls_file)
target_compile_definitions(test_zm_hls_file PRIVATE
    ZMS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zms_add_test(test_zm_ts_file)
target_compile_definitions(test_zm_ts_file PRIVATE
    ZMS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")
zms_add_test(test_zm_sdp)
zms_add_test(test_device_core)
zms_add_test(test_onvif_uri)
zms_add_test(test_device_xml)
zms_add_test(test_g711_mute_audio)
zms_add_test(test_base64)
zms_add_test(test_egress_clock)
zms_add_test(test_egress_pacing)
zms_add_test(test_http_request_reader)
zms_add_test(test_rtsp_range)
zms_add_test(test_config_ini)
target_compile_definitions(test_config_ini PRIVATE
    ZMS_CONFIG_INI="${PROJECT_SOURCE_DIR}/conf/config.ini")
zms_add_test(test_gb28181_ps)
zms_add_test(test_gb28181_sip)
zms_add_test(test_gb_media_loopback)
# WebRTC 关闭时 srtp 符号不参与链接，跳过该测试目标。
if (ZMS_ENABLE_WEBRTC)
    zms_add_test(test_webrtc_rtcp)
endif ()
zms_add_test(test_zm_mkv_write)
zms_add_test(test_zm_dash)
target_compile_definitions(test_zm_dash PRIVATE
    ZMS_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures")

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
