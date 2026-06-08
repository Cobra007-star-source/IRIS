// =============================================================================
// iris/jit.hpp  (Phase 3 骨架)
//
// JIT 引擎接口：
//   - 接收 CompiledSchema -> 在内存中生成 ARM NEON / x64 AVX2 校验机器码
//   - 通过 W^X 安全门完成 RW → RX 权限翻转
//   - Mac 上额外处理 MAP_JIT + pthread_jit_write_protect_np
//
// 当前是 stub：JitValidator::compile 返回空指针表示尚未实现，
// 上层 Validator 会自动 fallback 到解释执行的 Fast Path。
//
// 推荐后续接入：
//   - asmjit (https://asmjit.com) — header-only friendly
//   - cranelift (Rust，可走 cbindgen)
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

// Compiler API demo（验证 asmjit::Compiler 的虚拟寄存器分配 + 自动溢出）
using JitDemoFn = std::uint64_t (*)(std::uint64_t seed) noexcept;

// 一段 W^X 受控的可执行 trampoline。
// 不可拷贝，析构时归还 mmap 区域。
class IRIS_CACHE_ALIGNED ExecutableBuffer : public NonCopyable {
public:
    ExecutableBuffer() = default;
    ~ExecutableBuffer();

    ExecutableBuffer(ExecutableBuffer&&) noexcept;
    ExecutableBuffer& operator=(ExecutableBuffer&&) noexcept;

    // 申请一块 RW 内存，写入 N 字节代码后调用 freeze() 翻转为 RX。
    [[nodiscard]] static std::unique_ptr<ExecutableBuffer> create(std::size_t size);

    [[nodiscard]] std::uint8_t* writable() noexcept { return writable_; }
    [[nodiscard]] const std::uint8_t* code() const noexcept { return executable_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // 翻转权限：写入完成后调用。失败返回 false（错误信息打到 stderr）。
    [[nodiscard]] bool freeze() noexcept;

private:
    std::uint8_t* writable_   = nullptr;
    std::uint8_t* executable_ = nullptr;
    std::size_t   size_       = 0;
    bool          frozen_     = false;
};

// 把 CompiledSchema 编译为 JIT 函数。返回 nullptr 表示当前平台暂未实现，
// 上层应回退到解释 Fast Path。
[[nodiscard]] JitValidatorFn jit_compile(const CompiledSchema& schema,
                                         std::unique_ptr<ExecutableBuffer>& out_buffer) noexcept;

// Compiler API demo —— 用 asmjit::Compiler 生成一段会强制虚拟寄存器溢出
// 的函数，返回 (seed XOR 链尾值)。失败返回 nullptr。
[[nodiscard]] JitDemoFn jit_compile_compiler_demo() noexcept;

}  // namespace iris::jit
