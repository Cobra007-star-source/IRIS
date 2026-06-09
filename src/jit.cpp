// =============================================================================
// jit.cpp
// W^X executable buffer layer and asmjit codegen (Phase 3 skeleton)
// =============================================================================
#include "iris/jit.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
    #include <pthread.h>
    #include <TargetConditionals.h>
    #define IRIS_USE_MAP_JIT 1
#endif

// asmjit headers must be outside namespace iris::jit { } or
// `namespace asmjit { ... }` nests under iris::jit::asmjit.
#if defined(IRIS_HAVE_ASMJIT)
    #include <asmjit/core.h>
    #if defined(__aarch64__)
        #include <asmjit/a64.h>
    #elif defined(__x86_64__)
        #include <asmjit/x86.h>
    #endif
#endif

#include "iris/parser.hpp"

namespace iris::jit {

ExecutableBuffer::~ExecutableBuffer() {
    if (writable_) {
        ::munmap(writable_, size_);
    }
}

ExecutableBuffer::ExecutableBuffer(ExecutableBuffer&& o) noexcept {
    writable_   = o.writable_;   o.writable_   = nullptr;
    executable_ = o.executable_; o.executable_ = nullptr;
    size_       = o.size_;       o.size_       = 0;
    frozen_     = o.frozen_;     o.frozen_     = false;
}

ExecutableBuffer& ExecutableBuffer::operator=(ExecutableBuffer&& o) noexcept {
    if (this != &o) {
        if (writable_) ::munmap(writable_, size_);
        writable_   = o.writable_;   o.writable_   = nullptr;
        executable_ = o.executable_; o.executable_ = nullptr;
        size_       = o.size_;       o.size_       = 0;
        frozen_     = o.frozen_;     o.frozen_     = false;
    }
    return *this;
}

std::unique_ptr<ExecutableBuffer> ExecutableBuffer::create(std::size_t size) {
    long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    std::size_t rounded = (size + page - 1) & ~static_cast<std::size_t>(page - 1);

    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(IRIS_USE_MAP_JIT)
    flags |= MAP_JIT;
#endif

    void* p = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr, "[iris/jit] mmap failed: %s\n", std::strerror(errno));
        return nullptr;
    }

    auto buf = std::unique_ptr<ExecutableBuffer>(new ExecutableBuffer());
    buf->writable_   = static_cast<std::uint8_t*>(p);
    buf->executable_ = buf->writable_;
    buf->size_       = rounded;

#if defined(IRIS_USE_MAP_JIT)
    // enter write mode: MAP_JIT pages writable only
    ::pthread_jit_write_protect_np(0);
#endif

    return buf;
}

bool ExecutableBuffer::freeze() noexcept {
    if (frozen_) return true;

#if defined(IRIS_USE_MAP_JIT)
    // flip back to execute mode
    ::pthread_jit_write_protect_np(1);
#else
    if (::mprotect(writable_, size_, PROT_READ | PROT_EXEC) != 0) {
        std::fprintf(stderr, "[iris/jit] mprotect RX failed: %s\n", std::strerror(errno));
        return false;
    }
#endif

#if defined(__aarch64__)
    // sync instruction and data caches
    __builtin___clear_cache(reinterpret_cast<char*>(writable_),
                            reinterpret_cast<char*>(writable_) + size_);
#endif

    frozen_ = true;
    return true;
}

#if defined(IRIS_HAVE_ASMJIT)
// ============================================================================
// JIT codegen v0.2 — dual track:
//
//   (A) Trampoline (BaseAssembler) — still for generic schemas.
//       Tail-jmp forwards (data, size, schema, sret) to static interpreter.
//       Natural fit for JitValidatorFn (24-byte ValidationReport via sret):
//       sret on ARM64 via x8; asmjit::Compiler regalloc cannot model implicit sret,
//       so highest-performance path stays hand-written BaseAssembler.
//       (continued from above)
//
//   (B) Compiler API (BaseCompiler) — new.
//       Demo virtual reg allocation + auto spill in jit_compile_compiler_demo().
//       Allocates 32 virtual GP regs, chained XOR keeps all live,
//       returns sum. 32 > physical GPs (ARM64=29 callee-clobber
//       x86_64=14); asmjit spills excess virtual regs to stack,
//       reloads before use.
//
//       Not used by Validator directly — scaffolding for schema-specialized
//       codegen: perfect-hash field chain as 32+ virtual regs; asmjit handles
//       frame, save/restore, clobber sets.
//
// Register spill strategy (FAQ)
// -----------------------------
//   ARM64 hot path: ≤ 8 live virtual regs per path
//   (NEON string scan v0-v3, int range w0-w4); no spill. Schema
//   with 64 fields still sequential compare — reuse 1–2 key regs +
//   1–2 value regs + seen-mask on sret.
//   x86_64 (16 GP) same. Hardware reg exhaustion unlikely in this codegen shape;
//   asmjit spill is safety net not hot path.
//
//   Rare spill path: asmjit RAPass allocates stack slots by liveness,
//   reloads at use sites. No interpreter fallback; JIT still completes,
//   with extra spill/reload vs all-register form. Validator unchanged.
//   (continued)
// ============================================================================
namespace {

::asmjit::JitRuntime& jit_runtime() {
    static ::asmjit::JitRuntime rt;
    return rt;
}

ValidationReport jit_dispatch_to_interpreter(const std::uint8_t* data,
                                             std::size_t size,
                                             const CompiledSchema& schema) noexcept {
    return ::iris::validate(data, size, schema);
}

}  // namespace

// ----------------------------------------------------------------------------
// (A) BaseAssembler trampoline —— Validator hot path
// ----------------------------------------------------------------------------
JitValidatorFn jit_compile(const CompiledSchema& /*schema*/,
                           std::unique_ptr<ExecutableBuffer>& /*out_buffer*/) noexcept {
    ::asmjit::JitRuntime& rt = jit_runtime();
    ::asmjit::CodeHolder  code;
    code.init(rt.environment(), rt.cpu_features());

    auto fn_addr = reinterpret_cast<std::uintptr_t>(&jit_dispatch_to_interpreter);

#if defined(__aarch64__)
    ::asmjit::a64::Assembler a(&code);
    // x17 is ARM64 IP1 scratch; trampoline does not clash with sret(x8) / args(x0–x3).
    a.mov(::asmjit::a64::regs::x17, ::asmjit::Imm(static_cast<std::uint64_t>(fn_addr)));
    a.br (::asmjit::a64::regs::x17);
#elif defined(__x86_64__)
    ::asmjit::x86::Assembler a(&code);
    a.mov(::asmjit::x86::rax, ::asmjit::Imm(static_cast<std::uint64_t>(fn_addr)));
    a.jmp(::asmjit::x86::rax);
#else
    return nullptr;
#endif

    JitValidatorFn fn = nullptr;
    ::asmjit::Error err = rt.add(&fn, &code);
    if (err != ::asmjit::Error::kOk) {
        std::fprintf(stderr, "[iris/jit] asmjit add failed: %u\n",
                     static_cast<unsigned>(err));
        return nullptr;
    }
    return fn;
}

// ----------------------------------------------------------------------------
// (B) Compiler API demo — virtual reg allocation + auto spill
// ----------------------------------------------------------------------------
//
// Signature: std::uint64_t f(std::uint64_t seed)
// Behavior: 32-step ROR/XOR chain from seed; all 32 intermediates must stay live
//            forcing asmjit RAPass to spill/reload.
//            spill registers to stack and reload on use.
//
// Not called by Validator; purposes: (1) end-to-end Compiler API pipeline,
// (2) unit test spill correctness, (3) schema-specialized codegen scaffold.
// (duplicate line removed in translation)
// (scaffold)
//
JitDemoFn jit_compile_compiler_demo() noexcept {
    ::asmjit::JitRuntime& rt = jit_runtime();
    ::asmjit::CodeHolder  code;
    code.init(rt.environment(), rt.cpu_features());

#if defined(__aarch64__)
    ::asmjit::a64::Compiler cc(&code);

    auto sig = ::asmjit::FuncSignature::build<std::uint64_t, std::uint64_t>();
    ::asmjit::FuncNode* func = cc.add_func(sig);

    auto seed = cc.new_gpx("seed");
    func->set_arg(0, seed);

    // 32 virtual regs; ARM64 has only 31 GPs — spill required.
    constexpr int N = 32;
    ::asmjit::a64::Gp vregs[N];
    for (int i = 0; i < N; ++i) vregs[i] = cc.new_gpx("v");

    // Chain: vregs[i] = ROR(vregs[i-1], 1) + i
    // ROR breaks XOR symmetry (even/odd cancellation in long XOR chains),
// ADD immediate injects entropy; i ∈ [1,32) fits ADD imm.
    cc.mov(vregs[0], seed);
    for (int i = 1; i < N; ++i) {
        cc.ror(vregs[i], vregs[i-1], 1);
        cc.add(vregs[i], vregs[i], ::asmjit::Imm(static_cast<std::uint64_t>(i)));
    }
    // acc = v0 ^ v1 ^ ... ^ v31 — all vregs live until ret
    auto acc = cc.new_gpx("acc");
    cc.mov(acc, vregs[0]);
    for (int i = 1; i < N; ++i) cc.eor(acc, acc, vregs[i]);
    cc.ret(acc);

    cc.end_func();
    if (cc.finalize() != ::asmjit::Error::kOk) return nullptr;

#elif defined(__x86_64__)
    ::asmjit::x86::Compiler cc(&code);

    auto sig = ::asmjit::FuncSignature::build<std::uint64_t, std::uint64_t>();
    ::asmjit::FuncNode* func = cc.add_func(sig);

    auto seed = cc.new_gp64("seed");
    func->set_arg(0, seed);

    constexpr int N = 32;
    ::asmjit::x86::Gp vregs[N];
    for (int i = 0; i < N; ++i) vregs[i] = cc.new_gp64("v");

    cc.mov(vregs[0], seed);
    for (int i = 1; i < N; ++i) {
        cc.mov(vregs[i], vregs[i-1]);
        cc.ror(vregs[i], 1);
        cc.add(vregs[i], ::asmjit::Imm(i));
    }
    auto acc = cc.new_gp64("acc");
    cc.mov(acc, vregs[0]);
    for (int i = 1; i < N; ++i) cc.xor_(acc, vregs[i]);
    cc.ret(acc);

    cc.end_func();
    if (cc.finalize() != ::asmjit::Error::kOk) return nullptr;

#else
    return nullptr;
#endif

    JitDemoFn fn = nullptr;
    ::asmjit::Error err = rt.add(&fn, &code);
    if (err != ::asmjit::Error::kOk) {
        std::fprintf(stderr, "[iris/jit] compiler-demo add failed: %u\n",
                     static_cast<unsigned>(err));
        return nullptr;
    }
    return fn;
}

#else  // !IRIS_HAVE_ASMJIT

JitValidatorFn jit_compile(const CompiledSchema& /*schema*/,
                           std::unique_ptr<ExecutableBuffer>& /*out_buffer*/) noexcept {
    return nullptr;
}
JitDemoFn jit_compile_compiler_demo() noexcept { return nullptr; }

#endif

}  // namespace iris::jit
