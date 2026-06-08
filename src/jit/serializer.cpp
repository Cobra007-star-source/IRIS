// =============================================================================
// src/jit/serializer.cpp
//
// JIT serializer codegen. Compiles a constant payload into machine code that
// writes it into a caller-provided char* buffer.
//
// THE ARCHITECT'S QUESTION: byte-by-byte stores vs wide block writes?
// ---------------------------------------------------------------------------
// We do BLOCK WRITES, and the tail trick makes them branch-free.
//
//   Byte-by-byte (`mov byte [rdi], '{'` x27) costs ~27 store uops, ~27 stream
//   bytes of icache, and hammers a single store port. For a 27-byte body that
//   is the difference between ~27 stores and 4.
//
//   Block write: pack the literal into 64-bit words and store 8 bytes per
//   instruction. Neither x86-64 nor arm64 can store an arbitrary 64-bit
//   immediate straight to memory (x86 `mov m64, imm` only takes a sign-extended
//   imm32; arm64 has no store-immediate at all), so we first materialize the
//   word into a scratch GPR (x86 `movabs`, arm64 `movz`/`movk` chain) and then
//   issue one wide store. That is ~2 instructions per 8 bytes instead of 1 per
//   byte.
//
//   The unaligned tail (len % 8 != 0) would normally force a 4/2/1 descent. We
//   avoid it entirely with an OVERLAPPING store: write the final 8 bytes at
//   offset len-8. It overlaps the previous chunk but lands exactly on the last
//   byte, so a 27-byte body becomes exactly four 8-byte stores (offsets 0, 8,
//   16, 19) with zero branches. The buffer only needs `len` bytes of room
//   because the overlapping store ends at len.
//
//   Next level (not yet done): embed the literal in a rodata island and emit
//   PC-relative 16-byte SIMD stores (arm64 `ldr q`/`str q`, x86 `movdqu`),
//   halving the store count again. Deferred -- it needs a constant pool + reloc
//   and the GPR block write already collapses the route to a few uops.
// =============================================================================
#include "iris/jit/serializer.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// asmjit headers must live outside namespace iris::jit so its own
// `namespace asmjit { ... }` is not nested into iris::jit::asmjit.
#if defined(IRIS_HAVE_ASMJIT)
    #include <asmjit/core.h>
    #if defined(__aarch64__)
        #include <asmjit/a64.h>
    #elif defined(__x86_64__)
        #include <asmjit/x86.h>
    #endif
#endif

namespace iris::jit {

#if defined(IRIS_HAVE_ASMJIT)
namespace {

// One process-wide runtime owns the executable pages; JsonSerializer::release()
// hands functions back to it. Matches the pattern in src/jit.cpp.
::asmjit::JitRuntime& serializer_runtime() {
    static ::asmjit::JitRuntime rt;
    return rt;
}

// Little-endian load of n (<=8) bytes. x86-64 and arm64 are both little-endian,
// so storing this value back reproduces the source bytes verbatim.
std::uint64_t load_le(const char* p, std::size_t n) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, p, n);
    return v;
}

// A store the codegen will emit: write `width` bytes (1/2/4/8) of `value` to
// dst + offset.
struct Store {
    std::size_t   offset;
    std::uint8_t  width;
    std::uint64_t value;
};

// Plan the block write of `lit`: aligned 8-byte chunks, then a single
// overlapping 8-byte tail (len>=8) or a 4/2/1 descent (len<8).
void plan_block_write(std::string_view lit, std::vector<Store>& out) {
    const std::size_t len = lit.size();
    const char*       p   = lit.data();
    if (len == 0) return;

    const std::size_t full = (len / 8) * 8;
    for (std::size_t off = 0; off + 8 <= full; off += 8) {
        out.push_back({off, 8, load_le(p + off, 8)});
    }

    if (len % 8 != 0) {
        if (len >= 8) {
            const std::size_t off = len - 8;  // overlapping tail store
            out.push_back({off, 8, load_le(p + off, 8)});
        } else {
            std::size_t off = 0, rem = len;
            while (rem >= 4) { out.push_back({off, 4, load_le(p + off, 4)}); off += 4; rem -= 4; }
            if    (rem >= 2) { out.push_back({off, 2, load_le(p + off, 2)}); off += 2; rem -= 2; }
            if    (rem >= 1) { out.push_back({off, 1, load_le(p + off, 1)}); }
        }
    }
}

}  // namespace
#endif  // IRIS_HAVE_ASMJIT

// -----------------------------------------------------------------------------
// JsonSerializer
// -----------------------------------------------------------------------------
JsonSerializer::~JsonSerializer() { release(); }

JsonSerializer::JsonSerializer(JsonSerializer&& o) noexcept
    : fn_(o.fn_), length_(o.length_) {
    o.fn_     = nullptr;
    o.length_ = 0;
}

JsonSerializer& JsonSerializer::operator=(JsonSerializer&& o) noexcept {
    if (this != &o) {
        release();
        fn_       = o.fn_;
        length_   = o.length_;
        o.fn_     = nullptr;
        o.length_ = 0;
    }
    return *this;
}

void JsonSerializer::release() noexcept {
#if defined(IRIS_HAVE_ASMJIT)
    if (fn_) {
        serializer_runtime().release(fn_);
        fn_     = nullptr;
        length_ = 0;
    }
#endif
}

bool JsonSerializer::compile(std::string_view literal) noexcept {
#if defined(IRIS_HAVE_ASMJIT) && (defined(__aarch64__) || defined(__x86_64__))
    release();

    // Offsets are encoded as small immediates; cap the payload so we never feed
    // the encoder an out-of-range displacement. The gateway bodies are tens of
    // bytes -- anything larger should use the C++ serializer anyway.
    if (literal.size() > 4096) return false;

    std::vector<Store> stores;
    plan_block_write(literal, stores);

    const std::size_t len = literal.size();

    ::asmjit::JitRuntime& rt = serializer_runtime();
    ::asmjit::CodeHolder  code;
    code.init(rt.environment(), rt.cpu_features());

  #if defined(__aarch64__)
    // size_t fn(char* dst): dst in x0 (AAPCS64), return in x0. x9 is a caller-
    // saved temporary used to materialize each word. We must keep x0 = dst
    // until every store is issued, then overwrite x0 with len for the return.
    namespace a64 = ::asmjit::a64;
    a64::Assembler a(&code);
    const auto dst = a64::regs::x0;
    for (const Store& s : stores) {
        switch (s.width) {
            case 8:
                a.mov(a64::regs::x9, ::asmjit::Imm(s.value));  // movz/movk chain
                a.str(a64::regs::x9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
            case 4:
                a.mov(a64::regs::w9, ::asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                a.str(a64::regs::w9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
            case 2:
                a.mov(a64::regs::w9, ::asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                a.strh(a64::regs::w9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
            default:
                a.mov(a64::regs::w9, ::asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                a.strb(a64::regs::w9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
        }
    }
    a.mov(dst, ::asmjit::Imm(len));      // x0 = return value (after all stores read x0)
    a.ret(a64::regs::x30);
  #elif defined(__x86_64__)
    // size_t fn(char* dst): dst in rdi (SysV), return in rax. rdx is a scratch
    // for 64-bit immediates (x86 can store imm<=32 bits straight to memory).
    namespace x86 = ::asmjit::x86;
    x86::Assembler a(&code);
    for (const Store& s : stores) {
        switch (s.width) {
            case 8:
                a.mov(x86::rdx, ::asmjit::Imm(s.value));       // movabs rdx, imm64
                a.mov(x86::qword_ptr(x86::rdi, static_cast<int32_t>(s.offset)), x86::rdx);
                break;
            case 4:
                a.mov(x86::dword_ptr(x86::rdi, static_cast<int32_t>(s.offset)),
                      ::asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                break;
            case 2:
                a.mov(x86::word_ptr(x86::rdi, static_cast<int32_t>(s.offset)),
                      ::asmjit::Imm(static_cast<std::uint16_t>(s.value)));
                break;
            default:
                a.mov(x86::byte_ptr(x86::rdi, static_cast<int32_t>(s.offset)),
                      ::asmjit::Imm(static_cast<std::uint8_t>(s.value)));
                break;
        }
    }
    a.mov(x86::rax, ::asmjit::Imm(len));
    a.ret();
  #endif

    SerializeFn fn = nullptr;
    ::asmjit::Error err = rt.add(&fn, &code);
    if (err != ::asmjit::Error::kOk) {
        std::fprintf(stderr, "[iris/jit] serializer add failed: %u\n",
                     static_cast<unsigned>(err));
        return false;
    }

    fn_     = fn;
    length_ = len;
    return true;
#else
    (void)literal;
    return false;
#endif
}

}  // namespace iris::jit
