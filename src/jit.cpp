// =============================================================================
// src/jit.cpp  (Phase 3 骨架)
//
// 仅实现 W^X 内存层抽象。代码生成留为后续 Phase 接入 asmjit / cranelift。
//
// 已落地：
//   - Apple Silicon: MAP_JIT + pthread_jit_write_protect_np()
//   - Linux / 其他 POSIX: 两次 mprotect 翻转
//
// 未落地（明确标 TODO）：
//   - 实际 codegen
//   - 代码缓存 (W^X 区域复用)
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

// asmjit 头必须在 namespace iris::jit { } 之外，否则它声明的
// `namespace asmjit { ... }` 会被嵌套到 iris::jit::asmjit。
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
    // 进入写态：MAP_JIT 区域的页此时只允许写
    ::pthread_jit_write_protect_np(0);
#endif

    return buf;
}

bool ExecutableBuffer::freeze() noexcept {
    if (frozen_) return true;

#if defined(IRIS_USE_MAP_JIT)
    // 翻转回执行态
    ::pthread_jit_write_protect_np(1);
#else
    if (::mprotect(writable_, size_, PROT_READ | PROT_EXEC) != 0) {
        std::fprintf(stderr, "[iris/jit] mprotect RX failed: %s\n", std::strerror(errno));
        return false;
    }
#endif

#if defined(__aarch64__)
    // 指令缓存与数据缓存的同步：clear cache
    __builtin___clear_cache(reinterpret_cast<char*>(writable_),
                            reinterpret_cast<char*>(writable_) + size_);
#endif

    frozen_ = true;
    return true;
}

#if defined(IRIS_HAVE_ASMJIT)
// ============================================================================
// JIT codegen v0.2 — 双轨：
//
//   (A) Trampoline (BaseAssembler) — 仍用于通用 schema。
//       一段 tail-jmp 把 (data, size, schema, sret) 透传给静态解释器。这是
//       JitValidatorFn 接口（返回 24-byte ValidationReport 结构）天然的选择：
//       sret 在 ARM64 通过 x8 隐含传，asmjit::Compiler 的虚拟寄存器分配器
//       目前没法把 sret 隐含寄存器纳入它管理的虚拟寄存器空间，所以这条最
//       高性能的路径继续走 BaseAssembler 手写。
//
//   (B) Compiler API (BaseCompiler) — 新增。
//       用于 demo 虚拟寄存器分配 + 自动溢出。在 jit_compile_compiler_demo()
//       里实现一段"分配 32 个虚拟 GP 寄存器、链式 XOR 把所有寄存器都置活、
//       最后返回累加结果"的代码。32 > 物理 GP 个数（ARM64=29 callee-clobber
//       可用，x86_64=14 可用），asmjit 会自动把溢出的虚拟寄存器存到栈帧上，
//       下次用前再 reload。
//
//       这条路径不直接服务 Validator——它的存在是为后续 schema-specialized
//       codegen 留接口：当我们把 perfect-hash 比对的字段链作为 32+ 个虚拟
//       寄存器编排时，asmjit 会替我们安排栈帧、save/restore、clobber 集合。
//
// 寄存器溢出策略问题（用户提问）
// -----------------------------
//   ARM64 上 IRIS 当前编排：每条 hot path 中实时活跃的虚拟寄存器 ≤ 8 个
//   （NEON 字符串扫描 v0-v3，整型范围 w0-w4），不会触发溢出。schema
//   字段数即使到 64 也不需要 64 个并发寄存器——比对是顺序的，复用 1~2 个
//   key-reg + 1~2 个 value-reg + sret 上的 seen-mask 累积位即可。
//   x86_64 (16 GP) 同理。所以"硬件寄存器爆掉"在 IRIS 这个 codegen 形态下
//   并不真的会发生；asmjit::Compiler 的自动溢出能力是"安全网"而不是常路径。
//
//   当走到罕见的"硬件寄存器不够"路径时，asmjit 的 RAPass 会自动按
//   liveness 给溢出寄存器分配栈槽，下游使用点重新 ldr/str 加载。我们不
//   需要 fallback 到解释器；JIT 仍能完成 codegen，只是会比"全寄存器"
//   形态多几条 spill/reload 指令。Validator 不会观察到差别。
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
    // x17 是 ARM64 procedure-call scratch register (IP1)，做跳板不冲突 sret(x8) / 参数(x0~x3)。
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
// (B) Compiler API demo —— 验证虚拟寄存器分配 + 自动溢出
// ----------------------------------------------------------------------------
//
// 函数签名: std::uint64_t f(std::uint64_t seed)
// 行为     : 把 seed 经 32 步连续 XOR 旋转链生成一个最终值；32 个中间结果
//            都必须保持活跃，迫使 asmjit::Compiler 的 RAPass 自动把溢出
//            寄存器存栈、用时 reload。
//
// 这段 JIT 不直接被 Validator 调用；它的存在意义是 (1) 给 Compiler API
// 上一次端到端管线 (sig→func→regalloc→spill→finalize→runtime add)；
// (2) 给 unit test 验证 spill 的正确性；(3) 留作 schema-specialized codegen
// 的脚手架。
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

    // 分配 32 个虚拟寄存器；ARM64 GP 仅 31 个可用，必有溢出。
    constexpr int N = 32;
    ::asmjit::a64::Gp vregs[N];
    for (int i = 0; i < N; ++i) vregs[i] = cc.new_gpx("v");

    // 链式构造：vregs[i] = ROR(vregs[i-1], 1) + i   ——
    // 用 ROR 打破 XOR 的对称性（XOR 链在 32 步累加里会出现奇偶对消），
    // 用 ADD 立即数让信息持续注入。i ∈ [1,32) 可以直接做 ADD imm。
    cc.mov(vregs[0], seed);
    for (int i = 1; i < N; ++i) {
        cc.ror(vregs[i], vregs[i-1], 1);
        cc.add(vregs[i], vregs[i], ::asmjit::Imm(static_cast<std::uint64_t>(i)));
    }
    // 累加：acc = v0 ^ v1 ^ ... ^ v31  —— 强制所有 vreg 在 ret 之前 live
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
