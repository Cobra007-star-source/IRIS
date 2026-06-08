// =============================================================================
// iris/http/buffer.hpp
//
// Non-owning, fixed-capacity append buffer. Storage is owned by the caller
// (typically a Connection in the event loop). Every append is bounds-checked;
// on overflow the write is dropped and overflow() latches true so the caller
// can fail the request cleanly instead of corrupting memory.
//
// The buffer never allocates: it is the building block of the gateway's
// zero-allocation steady state.
// =============================================================================
#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

namespace iris::http {

class Buffer {
public:
    Buffer() = default;
    Buffer(char* data, std::size_t cap) noexcept : data_(data), cap_(cap) {}

    // Wrap existing storage that already holds `initial_len` bytes (used when
    // appending more responses onto a connection's pending write buffer).
    Buffer(char* data, std::size_t cap, std::size_t initial_len) noexcept
        : data_(data), cap_(cap), len_(initial_len) {}

    [[nodiscard]] char*        data()       noexcept { return data_; }
    [[nodiscard]] const char*  data() const noexcept { return data_; }
    [[nodiscard]] std::size_t  size()      const noexcept { return len_; }
    [[nodiscard]] std::size_t  capacity()  const noexcept { return cap_; }
    [[nodiscard]] std::size_t  remaining() const noexcept { return cap_ - len_; }
    [[nodiscard]] bool         overflow()  const noexcept { return overflow_; }
    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(data_, len_);
    }

    void clear() noexcept { len_ = 0; overflow_ = false; }

    void append(std::string_view s) noexcept {
        if (s.size() > cap_ - len_) { overflow_ = true; return; }
        std::memcpy(data_ + len_, s.data(), s.size());
        len_ += s.size();
    }

    void append(char c) noexcept {
        if (len_ >= cap_) { overflow_ = true; return; }
        data_[len_++] = c;
    }

    // Append an unsigned decimal integer with no allocation.
    void append_uint(std::size_t v) noexcept {
        char tmp[20];
        int n = 0;
        if (v == 0) { append('0'); return; }
        while (v) { tmp[n++] = static_cast<char>('0' + v % 10); v /= 10; }
        if (static_cast<std::size_t>(n) > cap_ - len_) { overflow_ = true; return; }
        while (n) data_[len_++] = tmp[--n];
    }

private:
    char*       data_     = nullptr;
    std::size_t cap_      = 0;
    std::size_t len_      = 0;
    bool        overflow_ = false;
};

}  // namespace iris::http
