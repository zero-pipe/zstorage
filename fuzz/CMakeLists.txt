# ZMS fuzz 靶。默认不构建；用 -DZMS_BUILD_FUZZ=ON 且编译器为 clang 时启用。
#
# 关键：库对象也必须带 sanitizer 插桩，否则只有靶文件有覆盖率、崩溃只能靠
# malloc 拦截。因此整个构建用全局 sanitizer 标志配置（见下），靶只额外加
# libFuzzer 主程序链接。
#
#   cmake -S . -B build-fuzz -DZMS_BUILD_FUZZ=ON -DCMAKE_C_COMPILER=clang #         -DCMAKE_BUILD_TYPE=Debug #         -DCMAKE_C_FLAGS="-fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -g" #         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=fuzzer-no-link,address,undefined"
#   cmake --build build-fuzz --target fuzz_parsers
#   ./build-fuzz/fuzz/fuzz_parsers -max_total_time=300 fuzz/corpus

option(ZMS_BUILD_FUZZ "Build libFuzzer targets (requires clang)" OFF)

if(ZMS_BUILD_FUZZ)
    add_executable(fuzz_parsers fuzz_parsers.c)
    target_include_directories(fuzz_parsers PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(fuzz_parsers PRIVATE zms znet zmedia_core)
    # 靶本身需要 fuzzer 主程序；库对象由全局 CMAKE_C_FLAGS 插桩（见文件头注释）。
    target_compile_options(fuzz_parsers PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_parsers PRIVATE -fsanitize=fuzzer)

    # 种子语料（覆盖每个靶的一个典型报文）
    set(FUZZ_CORPUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/corpus)

    # 冒烟：离线跑 15 秒，确认靶本身可用且无即时崩溃（CI 用）
    add_test(NAME fuzz_parsers_smoke
             COMMAND fuzz_parsers -max_total_time=15 -rss_limit_mb=2048
                     -detect_leaks=0 ${FUZZ_CORPUS_DIR})
endif()
