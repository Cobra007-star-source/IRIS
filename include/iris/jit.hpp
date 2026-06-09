// =============================================================================
// iris/jit.hpp  (Phase 3 skeleton)
//
// JIT engine interface:
//   - Accept CompiledSchema -> generate ARM NEON / x64 AVX2 validation machine code
//   - W^X safety gate: RW -> RX permission flip
//   - macOS: MAP_JIT + pthread_jit_write_protect_np
//
// Currently stub: JitValidator::compile returns nullptr when not implemented;
// upper Validator falls back to interpreted Fast Path.
//
// Recommended future integrations:
//   - asmjit (https://asmjit.com) — header-only friendly
//   - cranelift (Rust, via cbindgen)
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "iris/common.hpp"
#include "iris/parser.hpp"
#include "iris/schema.hpp"

namespace iris::jit {

using JitValidatorFn = ValidationReport (*)(const std::uint8_t* data,
                                            std::size_t size,
                                            const CompiledSchema& schema) noexcept;

// Compiler API demo (verify asmjit::Compiler virtual register allocation + auto spill)
using JitDemoFn = std::uint64_t (*)(std::uint64_t seed) noexcept;

// W^X-controlled executable trampoline.
// Non-copyable; returns mmap region on destroy.
class IRIS_CACHE_ALIGNED ExecutableBuffer : public NonCopyable {
public:
    ExecutableBuffer() = default;
    ~ExecutableBuffer();

    ExecutableBuffer(ExecutableBuffer&&) noexcept;
    ExecutableBuffer& operator=(ExecutableBuffer&&) noexcept;

    // Allocate RW memory, write N bytes, then freeze() to RX.
    [[nodiscard]] static std::unique_ptr<ExecutableBuffer> create(std::size_t size);

    [[nodiscard]] std::uint8_t* writable() noexcept { return writable_; }
    [[nodiscard]] const std::uint8_t* code() const noexcept { return executable_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Flip permissions after writing. Returns false on failure (error to stderr).
    [[nodiscard]] bool freeze() noexcept;

private:
    std::uint8_t* writable_   = nullptr;
    std::uint8_t* executable_ = nullptr;
    std::size_t   size_       = 0;
    bool          frozen_     = false;
};

// Compile CompiledSchema to JIT function. nullptr means not implemented on this platform;
// caller should fall back to interpreted Fast Path.
[[nodiscard]] JitValidatorFn jit_compile(const CompiledSchema& schema,
                                         std::unique_ptr<ExecutableBuffer>& out_buffer) noexcept;

// Compiler API demo — asmjit::Compiler generates a function that forces virtual register
// spill; returns (seed XOR chain tail). nullptr on failure.
[[nodiscard]] JitDemoFn jit_compile_compiler_demo() noexcept;

}  // namespace iris::jit
