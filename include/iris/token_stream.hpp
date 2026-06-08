// =============================================================================
// iris/token_stream.hpp
//
// SoA Token 流 (Phase 2)
//
// 关键决策：放弃 AoS 的 struct Token { TokenKind k; uint32_t off; uint32_t len; ... }
// 改用并行数组：
//
//   kinds : uint8_t[]   (1 byte/elem, 64 个一行 cache line)
//   offsets: uint32_t[] (4 byte/elem)
//   lengths: uint32_t[] (4 byte/elem)
//
// 这样:
//   - 仅做类型判断的状态机一次性 prefetch 64 个 kind，几乎全部命中 L1
//   - 仅做位置定位的代码不必为 kind 浪费带宽
//   - 类型分支可以用 SIMD 一次性比对 16 个 token
//
// Token 数组同样是 64 字节对齐的 over-aligned 分配，配合 std::span 暴露给上层。
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "iris/common.hpp"

namespace iris {

// 极简 token 集合：目标是覆盖 JSON 八种结构 token。
// 数值用 0..7 紧排，方便 8-bit 比较与跳转表。
enum class TokenKind : std::uint8_t {
    kObjectOpen   = 0,  // {
    kObjectClose  = 1,  // }
    kArrayOpen    = 2,  // [
    kArrayClose   = 3,  // ]
    kString       = 4,  // "..."
    kNumber       = 5,  // 12.3 / -1 / 1e9
    kBoolean      = 6,  // true / false
    kNull         = 7,  // null
    kInvalid      = 0xFF,
};

inline constexpr const char* token_kind_name(TokenKind k) noexcept {
    switch (k) {
        case TokenKind::kObjectOpen:  return "{";
        case TokenKind::kObjectClose: return "}";
        case TokenKind::kArrayOpen:   return "[";
        case TokenKind::kArrayClose:  return "]";
        case TokenKind::kString:      return "string";
        case TokenKind::kNumber:      return "number";
        case TokenKind::kBoolean:     return "boolean";
        case TokenKind::kNull:        return "null";
        default:                      return "invalid";
    }
}

// SoA 视图（不持有所有权，由 TokenStream 拥有）
struct TokenView {
    std::span<const std::uint8_t> kinds;
    std::span<const std::uint32_t> offsets;
    std::span<const std::uint32_t> lengths;

    [[nodiscard]] std::size_t size() const noexcept { return kinds.size(); }
};

// 拥有所有权、64 字节对齐的 token 流容器。
//
// 不是 std::vector：因为我们要：
//   1. 强制 64B 对齐（DOD）
//   2. 不抛异常、不调 ctor/dtor（TriviallyCopyable，POD）
//   3. 一次性 reserve，避免 realloc 抖动
class IRIS_CACHE_ALIGNED TokenStream : public NonCopyable {
public:
    TokenStream() = default;

    explicit TokenStream(std::size_t reserve_n) { reserve(reserve_n); }

    ~TokenStream() { release(); }

    TokenStream(TokenStream&& o) noexcept { steal_from(o); }
    TokenStream& operator=(TokenStream&& o) noexcept {
        if (this != &o) { release(); steal_from(o); }
        return *this;
    }

    void reserve(std::size_t n);

    IRIS_FORCE_INLINE void push(TokenKind k, std::uint32_t off, std::uint32_t len) {
        if (IRIS_UNLIKELY(size_ == cap_)) grow();
        kinds_[size_]   = static_cast<std::uint8_t>(k);
        offsets_[size_] = off;
        lengths_[size_] = len;
        ++size_;
    }

    IRIS_FORCE_INLINE void clear() noexcept { size_ = 0; }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return cap_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] TokenView view() const noexcept {
        return TokenView{
            std::span<const std::uint8_t>(kinds_, size_),
            std::span<const std::uint32_t>(offsets_, size_),
            std::span<const std::uint32_t>(lengths_, size_),
        };
    }

private:
    void grow();
    void release() noexcept;
    void steal_from(TokenStream& o) noexcept;

    std::uint8_t*  kinds_   = nullptr;
    std::uint32_t* offsets_ = nullptr;
    std::uint32_t* lengths_ = nullptr;
    std::size_t    size_    = 0;
    std::size_t    cap_     = 0;
};

}  // namespace iris
