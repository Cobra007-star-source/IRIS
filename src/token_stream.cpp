// =============================================================================
// token_stream.cpp
// Plain PoD container for SoA token storage
// =============================================================================
#include "iris/token_stream.hpp"

#include <algorithm>
#include <cstring>

namespace iris {

void TokenStream::reserve(std::size_t n) {
    if (n <= cap_) return;
    auto* new_kinds   = aligned_alloc_n<std::uint8_t>(n);
    auto* new_offsets = aligned_alloc_n<std::uint32_t>(n);
    auto* new_lengths = aligned_alloc_n<std::uint32_t>(n);
    if (size_) {
        std::memcpy(new_kinds,   kinds_,   size_ * sizeof(std::uint8_t));
        std::memcpy(new_offsets, offsets_, size_ * sizeof(std::uint32_t));
        std::memcpy(new_lengths, lengths_, size_ * sizeof(std::uint32_t));
    }
    aligned_free(kinds_);
    aligned_free(offsets_);
    aligned_free(lengths_);
    kinds_   = new_kinds;
    offsets_ = new_offsets;
    lengths_ = new_lengths;
    cap_     = n;
}

void TokenStream::grow() {
    reserve(cap_ ? cap_ * 2 : 64);
}

void TokenStream::release() noexcept {
    aligned_free(kinds_);   kinds_   = nullptr;
    aligned_free(offsets_); offsets_ = nullptr;
    aligned_free(lengths_); lengths_ = nullptr;
    size_ = cap_ = 0;
}

void TokenStream::steal_from(TokenStream& o) noexcept {
    kinds_   = o.kinds_;   o.kinds_   = nullptr;
    offsets_ = o.offsets_; o.offsets_ = nullptr;
    lengths_ = o.lengths_; o.lengths_ = nullptr;
    size_ = o.size_;       o.size_ = 0;
    cap_  = o.cap_;        o.cap_  = 0;
}

}  // namespace iris
