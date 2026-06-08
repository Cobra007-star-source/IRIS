// =============================================================================
// iris/jit/serializer.hpp
//
// JIT response serializer. Compiles the serialization of a *constant* payload
// (e.g. the /json response body {"message":"Hello, World!"}) into machine code
// via asmjit, so the gateway emits the body with a handful of wide register
// stores instead of walking a C++ struct field by field.
//
// This is the "physically erase the C++ serializer" path the gateway uses on
// the /json hot route. The generated function is straight-line, branch-free,
// and loop-free; see src/jit/serializer.cpp for the block-write rationale and
// the ISA-specific encoding (arm64 / x86-64).
// =============================================================================
#pragma once

#include <cstddef>
#include <string_view>

namespace iris::jit {

// A JIT-compiled serializer. Writes the compiled payload into `dst` and returns
// the number of bytes written. The caller guarantees `dst` has room for at
// least length() bytes (the generated code may use an overlapping tail store,
// so it never writes past length()).
using SerializeFn = std::size_t (*)(char* dst) noexcept;

class JsonSerializer {
public:
    JsonSerializer() = default;
    ~JsonSerializer();

    JsonSerializer(const JsonSerializer&)            = delete;
    JsonSerializer& operator=(const JsonSerializer&) = delete;
    JsonSerializer(JsonSerializer&&) noexcept;
    JsonSerializer& operator=(JsonSerializer&&) noexcept;

    // Compile a serializer for the fixed bytes in `literal`. Returns true on
    // success. Returns false when JIT codegen is unavailable -- a build without
    // asmjit, an unsupported ISA, or a payload whose offsets exceed the encoder
    // reach. On false the caller must fall back to the C++ serializer.
    [[nodiscard]] bool compile(std::string_view literal) noexcept;

    [[nodiscard]] SerializeFn fn() const noexcept { return fn_; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }
    [[nodiscard]] bool        ready() const noexcept { return fn_ != nullptr; }

private:
    void release() noexcept;

    SerializeFn fn_     = nullptr;
    std::size_t length_ = 0;
};

}  // namespace iris::jit
