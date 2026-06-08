# -----------------------------------------------------------------------------
# arch_flags.cmake
#
# 物理级优化必须依赖具体 ISA。这里集中决定 SIMD 指令集与代码生成基线，
# 避免在源代码里到处散布 #ifdef。
# -----------------------------------------------------------------------------

set(IRIS_ARCH_FLAGS "")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
    set(IRIS_TARGET_ISA "arm64-neon" CACHE INTERNAL "Detected SIMD ISA")
    # Apple Silicon / ARM64 默认开启 NEON，这里显式声明便于编译期断言
    list(APPEND IRIS_ARCH_FLAGS
        -march=armv8.4-a+fp16
        -ffast-math
        -fno-omit-frame-pointer
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
    set(IRIS_TARGET_ISA "x86_64-avx2" CACHE INTERNAL "Detected SIMD ISA")
    list(APPEND IRIS_ARCH_FLAGS
        -mavx2
        -mbmi2
        -mfma
        -ffast-math
        -fno-omit-frame-pointer
    )
else()
    set(IRIS_TARGET_ISA "scalar" CACHE INTERNAL "Detected SIMD ISA")
    message(WARNING "[iris] Unknown processor '${CMAKE_SYSTEM_PROCESSOR}', falling back to scalar.")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    list(APPEND IRIS_ARCH_FLAGS -O3 -DNDEBUG)
endif()

message(STATUS "[iris] Target ISA      : ${IRIS_TARGET_ISA}")
