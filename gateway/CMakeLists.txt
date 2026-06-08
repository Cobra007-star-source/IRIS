# -----------------------------------------------------------------------------
# gateway/CMakeLists.txt
#
# iris_net : platform networking + HTTP/1.1 framing (separate from the validator
#            core so the engine library stays free of network dependencies).
# iris-gw  : the TechEmpower gateway executable wiring routes onto iris_net.
#
# picohttpparser is fetched and compiled by the root CMakeLists before this
# subdirectory is added (target `picohttpparser`).
# -----------------------------------------------------------------------------

find_package(Threads REQUIRED)

add_library(iris_net STATIC
    ${CMAKE_SOURCE_DIR}/src/net/poller.cpp
    ${CMAKE_SOURCE_DIR}/src/net/server.cpp
    ${CMAKE_SOURCE_DIR}/src/http/parser.cpp
    ${CMAKE_SOURCE_DIR}/src/http/date.cpp
)
target_include_directories(iris_net PUBLIC ${CMAKE_SOURCE_DIR}/include)
target_compile_features(iris_net PUBLIC cxx_std_20)
target_compile_options(iris_net PRIVATE ${IRIS_ARCH_FLAGS})
target_link_libraries(iris_net PUBLIC picohttpparser Threads::Threads)

# Deployment profile: racing (epoll/SO_REUSEPORT/pinned) vs production (portable).
if(IRIS_PROFILE STREQUAL "racing")
    target_compile_definitions(iris_net PUBLIC IRIS_PROFILE_RACING=1)
else()
    target_compile_definitions(iris_net PUBLIC IRIS_PROFILE_PRODUCTION=1)
endif()

add_executable(iris-gw main.cpp)
target_link_libraries(iris-gw PRIVATE iris_net)
target_compile_options(iris-gw PRIVATE ${IRIS_ARCH_FLAGS})

# Optional JIT serializer for the /json route. Built only with -DIRIS_ENABLE_JIT=ON
# (asmjit is fetched by the root CMake before this subdirectory is added). The
# default racing build stays free of asmjit and runs the portable C++
# serializer; the JIT path is a drop-in fast path selected at runtime.
if(IRIS_ENABLE_JIT)
    target_sources(iris-gw PRIVATE ${CMAKE_SOURCE_DIR}/src/jit/serializer.cpp)
    target_link_libraries(iris-gw PRIVATE asmjit)
    target_compile_definitions(iris-gw PRIVATE IRIS_HAVE_ASMJIT=1)
endif()

# -----------------------------------------------------------------------------
# libFuzzer harness for the HTTP request parser (self-contained: instruments
# both the wrapper and picohttpparser.c with fuzzer + ASan). Only built where
# the libFuzzer runtime links (full clang on Linux/CI); absent from Apple's
# command-line clang, where iris_http_stress below is the local stand-in.
# -----------------------------------------------------------------------------
if(IRIS_BUILD_FUZZER)
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
    check_cxx_source_compiles(
        "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long){return 0;}"
        IRIS_HAVE_LIBFUZZER)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)

    if(IRIS_HAVE_LIBFUZZER)
        add_executable(iris_http_fuzz
            ${CMAKE_SOURCE_DIR}/fuzz/http_parser_fuzz.cpp
            ${CMAKE_SOURCE_DIR}/src/http/parser.cpp
            ${picohttpparser_SOURCE_DIR}/picohttpparser.c
        )
        target_include_directories(iris_http_fuzz PRIVATE
            ${CMAKE_SOURCE_DIR}/include
            ${picohttpparser_SOURCE_DIR}
        )
        target_compile_options(iris_http_fuzz PRIVATE -fsanitize=fuzzer,address -g -O1)
        target_link_options(iris_http_fuzz PRIVATE -fsanitize=fuzzer,address)
    else()
        message(STATUS "[iris] libFuzzer runtime unavailable; iris_http_stress only")
    endif()
endif()

# Portable robustness driver (local stand-in where the libFuzzer runtime is
# unavailable, e.g. Apple command-line clang). It pairs UBSan with the harness's
# explicit contract assertions. NOTE: Apple CLT's ASan/TSan runtimes hang or
# crash at startup on this arm64 toolchain, so heap-OOB coverage (ASan) comes
# from iris_http_fuzz on Linux/CI; UBSan works locally and is used here.
# Override with -DIRIS_STRESS_SANITIZER=address,undefined on a healthy toolchain.
if(IRIS_BUILD_FUZZER)
    set(IRIS_STRESS_SANITIZER "undefined" CACHE STRING
        "Sanitizer for iris_http_stress (ASan hangs on Apple CLT arm64)")
    add_executable(iris_http_stress
        ${CMAKE_SOURCE_DIR}/fuzz/http_parser_stress.cpp
        ${CMAKE_SOURCE_DIR}/src/http/parser.cpp
        ${picohttpparser_SOURCE_DIR}/picohttpparser.c
    )
    target_include_directories(iris_http_stress PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${picohttpparser_SOURCE_DIR}
    )
    target_compile_options(iris_http_stress PRIVATE
        -fsanitize=${IRIS_STRESS_SANITIZER} -fno-sanitize-recover=all -g -O2)
    target_link_options(iris_http_stress PRIVATE -fsanitize=${IRIS_STRESS_SANITIZER})
endif()
