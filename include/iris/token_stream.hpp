// =============================================================================
// iris/token_stream.hpp
//
// SoA token stream (Phase 2)
//
// Key decision: drop AoS struct Token { TokenKind k; uint32_t off; uint32_t len; ... }
// in favor of parallel arrays:
//
//   kinds : uint8_t[]   (1 byte/elem, 64 per cache line)
//   offsets: uint32_t[] (4 byte/elem)
//   lengths: uint32_t[] (4 byte/elem)
//
// Benefits:
//   - Type-only state machines prefetch 64 kinds at once, mostly L1 hits
//   - Position-only code does not waste bandwidth on kind
//   - Type branches can SIMD-compare 16 tokens at once
//
// Token arrays are 64-byte aligned over-aligned allocations, exposed upstream via std::span.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "iris/common.hpp"

namespace iris {

// Minimal token set: cover eight JSON structural tokens.
// Values 0..7 packed for 8-bit compare and jump tables.
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

// SoA view (non-owning; owned by TokenStream)
struct TokenView {
    std::span<const std::uint8_t> kinds;
    std::span<const std::uint32_t> offsets;
    std::span<const std::uint32_t> lengths;

    [[nodiscard]] std::size_t size() const noexcept { return kinds.size(); }
};

// Owning, 64-byte-aligned token stream container.
//
// Not std::vector because we need:
//   1. Forced 64B alignment (DOD)
//   2. No exceptions, no ctor/dtor (trivially copyable POD)
//   3. One-shot reserve to avoid realloc jitter
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
