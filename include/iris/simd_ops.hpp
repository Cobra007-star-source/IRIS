// =============================================================================
// iris/simd_ops.hpp
//
// SIMD 字节级原语 (Phase 1: 硅基物理直觉)
//
// 目标：
//   - 在原始 buffer 上零拷贝扫描某个字节出现的次数 / 位置
//   - 在 ARM64 上使用 128-bit NEON，在 x86_64 上使用 256-bit AVX2
//   - 提供 scalar 回退，作为正确性 oracle
//
// 这是整个引擎里最低层的“凿岩机”。后续的换行符统计、空白跳过、
// 字符串扫描、引号匹配都会在这个原语之上演化。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "iris/common.hpp"

namespace iris::simd {

// 返回当前编译目标使用的实现名（用于 benchmark 报告）。
const char* implementation_name() noexcept;

// 计数 buffer 中 byte == needle 的字节数。
//
// SoA 风格的纯函数：无副作用、零分配、栈上完成。
// 输入数据可以未对齐，函数内部会处理头/尾边角。
[[nodiscard]] std::size_t count_byte(const std::uint8_t* IRIS_RESTRICT data,
                                     std::size_t size,
                                     std::uint8_t needle) noexcept;

[[nodiscard]] IRIS_FORCE_INLINE std::size_t
count_byte(std::string_view view, std::uint8_t needle) noexcept {
    return count_byte(reinterpret_cast<const std::uint8_t*>(view.data()),
                      view.size(), needle);
}

// 标量参考实现：可被 benchmark 显式调用以观察 SIMD 加速比。
[[nodiscard]] std::size_t count_byte_scalar(const std::uint8_t* IRIS_RESTRICT data,
                                            std::size_t size,
                                            std::uint8_t needle) noexcept;

// 定位首个 needle 字节，未命中返回 size。
[[nodiscard]] std::size_t find_byte(const std::uint8_t* IRIS_RESTRICT data,
                                    std::size_t size,
                                    std::uint8_t needle) noexcept;

[[nodiscard]] std::size_t find_byte_scalar(const std::uint8_t* IRIS_RESTRICT data,
                                           std::size_t size,
                                           std::uint8_t needle) noexcept;

// 定位首个等于 a 或 b 的字节，未命中返回 size。
// 这是 scan_string 的关键加速器：单 SIMD pass 同时处理 '"' 和 '\\'。
[[nodiscard]] std::size_t find_byte_pair(const std::uint8_t* IRIS_RESTRICT data,
                                         std::size_t size,
                                         std::uint8_t a,
                                         std::uint8_t b) noexcept;

[[nodiscard]] std::size_t find_byte_pair_scalar(const std::uint8_t* IRIS_RESTRICT data,
                                                std::size_t size,
                                                std::uint8_t a,
                                                std::uint8_t b) noexcept;

// 跳过开头连续的 JSON 空白字符（' ', '\t', '\n', '\r'），返回首个非空白字符的偏移。
//
// 这是后续“凌空 Token 化”的入口，单独导出便于 benchmark 与单测复用。
// 完整 SIMD 实现（处理任意长度空白）。
[[nodiscard]] std::size_t skip_json_whitespace_full(const std::uint8_t* IRIS_RESTRICT data,
                                                    std::size_t size) noexcept;

// JSON 空白集合 {0x20, 0x09, 0x0A, 0x0D} 全部 ≤ 0x20。
// 紧凑 JSON 极常见，首字节 > 0x20 即可零代价短路。
//
// inline 在 header 是有意为之：scan/validate 热路径调用千万次，
// 跨 TU 边界即使 LTO 也不保证 inline 满。
[[nodiscard]] IRIS_FORCE_INLINE std::size_t
skip_json_whitespace(const std::uint8_t* IRIS_RESTRICT data, std::size_t size) noexcept {
    if (IRIS_LIKELY(size != 0 && data[0] > 0x20)) return 0;
    return skip_json_whitespace_full(data, size);
}

}  // namespace iris::simd
