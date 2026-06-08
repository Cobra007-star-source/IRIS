cmake_minimum_required(VERSION 3.20)
project(iris
    VERSION 0.1.0
    LANGUAGES CXX
    DESCRIPTION "Irisoul's Resolver for Inline Schema - SIMD JSON Schema validator"
)

# -----------------------------------------------------------------------------
# Global compile settings
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 开启 LTO 让 simd_ops / parser / validator 跨 TU 内联。
# Phase 1 已确认 NEON 单循环吞吐 33 GB/s；FastPath 校验吞吐瓶颈在
# 函数调用边界（无法跨 TU 内联）。LTO 一刀切掉这层。
if(NOT DEFINED IRIS_ENABLE_LTO)
    set(IRIS_ENABLE_LTO ON)
endif()
if(IRIS_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
    if(ipo_ok)
        message(STATUS "[iris] LTO/IPO   : enabled")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(STATUS "[iris] LTO/IPO   : unsupported (${ipo_msg})")
    endif()
endif()

include(cmake/arch_flags.cmake)

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(IRIS_BUILD_EXAMPLES   "Build IRIS example programs"  ON)
option(IRIS_BUILD_BENCHMARKS "Build IRIS micro-benchmarks"  ON)
option(IRIS_BUILD_TESTS      "Build IRIS unit tests"        ON)
option(IRIS_BUILD_SIMDJSON   "Fetch and link simdjson for cross-comparison" ON)
option(IRIS_BUILD_BOWTIE     "Build Bowtie protocol harness" ON)
option(IRIS_ENABLE_JIT       "Enable JIT code generation"   OFF)

# -----------------------------------------------------------------------------
# Core library: iris
# -----------------------------------------------------------------------------
add_library(iris STATIC
    src/simd_ops.cpp
    src/token_stream.cpp
    src/perfect_hash.cpp
    src/json_reader.cpp
    src/schema.cpp
    src/parser.cpp
    src/inspector.cpp
    src/slow_path.cpp
    src/slow_schema.cpp
    src/slow_eval.cpp
    src/jit.cpp
    src/validator.cpp
)

target_include_directories(iris PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_options(iris PRIVATE ${IRIS_ARCH_FLAGS})
target_compile_features(iris PUBLIC cxx_std_20)

# 慢车道走 IEEE-754 严格语义：multipleOf 的溢出检测依赖 isfinite()，被
# -ffast-math 假设掉就会出错。慢车道不在 hot path 上，安全比 perf 重要。
set_source_files_properties(src/slow_eval.cpp src/slow_schema.cpp
    PROPERTIES
    COMPILE_OPTIONS "-fno-fast-math;-fno-finite-math-only")

if(IRIS_ENABLE_JIT)
    target_compile_definitions(iris PUBLIC IRIS_ENABLE_JIT=1)
endif()

# -----------------------------------------------------------------------------
# Subprojects
# -----------------------------------------------------------------------------
if(IRIS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(IRIS_BUILD_SIMDJSON OR IRIS_ENABLE_JIT)
    include(FetchContent)
endif()

if(IRIS_BUILD_SIMDJSON)
    # simdjson 是纯解析 baseline。FetchContent 拉，关闭可用 -DIRIS_BUILD_SIMDJSON=OFF。
    FetchContent_Declare(
        simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson.git
        GIT_TAG        v3.10.1
        GIT_SHALLOW    TRUE
    )
    set(SIMDJSON_BUILD_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(SIMDJSON_DEVELOPER_MODE   OFF CACHE BOOL "" FORCE)
    set(SIMDJSON_JUST_LIBRARY     ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(simdjson)
endif()

if(IRIS_ENABLE_JIT)
    # asmjit：仅在 -DIRIS_ENABLE_JIT=ON 时拉取
    FetchContent_Declare(
        asmjit
        GIT_REPOSITORY https://github.com/asmjit/asmjit.git
        GIT_TAG        master
        GIT_SHALLOW    TRUE
    )
    set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(asmjit)
    target_link_libraries(iris PUBLIC asmjit)
    target_compile_definitions(iris PUBLIC IRIS_HAVE_ASMJIT=1)
endif()

# -----------------------------------------------------------------------------
# RE2: 慢车道的正则引擎。优先用系统安装（brew install re2 / apt install libre2-dev），
# 没装时退到 std::regex。RE2 是线性时间、ReDoS-safe；std::regex 在 Apple Clang
# 下 backtracking、可能在 adversarial pattern 上慢到不可接受。
# -----------------------------------------------------------------------------
find_package(re2 QUIET)
if(re2_FOUND)
    message(STATUS "[iris] re2          : found (slow-path uses RE2)")
    target_link_libraries(iris PUBLIC re2::re2)
    target_compile_definitions(iris PUBLIC IRIS_HAVE_RE2=1)
else()
    message(STATUS "[iris] re2          : not found (slow-path falls back to std::regex)")
endif()

if(IRIS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
    add_subdirectory(bench)
endif()

if(IRIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Print configuration summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=========== IRIS configuration summary ===========")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler        : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Arch flags      : ${IRIS_ARCH_FLAGS}")
message(STATUS "  Examples        : ${IRIS_BUILD_EXAMPLES}")
message(STATUS "  Benchmarks      : ${IRIS_BUILD_BENCHMARKS}")
message(STATUS "  Tests           : ${IRIS_BUILD_TESTS}")
message(STATUS "  JIT enabled     : ${IRIS_ENABLE_JIT}")
message(STATUS "==================================================")
message(STATUS "")
