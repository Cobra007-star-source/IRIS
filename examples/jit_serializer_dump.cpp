// =============================================================================
// examples/jit_serializer_dump.cpp
//
// Cross-ISA disassembly dump for the JIT JSON serializer. Emits the block-write
// code for {"message":"Hello, World!"} (27 bytes) for BOTH aarch64 and x86-64
// and prints the mnemonics + encoded machine-code bytes via asmjit's logger.
//
// asmjit is a cross-assembler: from any host it can ENCODE any supported ISA.
// We only log here (never execute the foreign arch), so this runs identically
// on an Apple-silicon Mac and on an x86 server.
//
// The emit logic mirrors src/jit/serializer.cpp exactly (block write + single
// overlapping tail store). Diagnostic only; not on any hot path.
// =============================================================================
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <asmjit/core.h>
#include <asmjit/a64.h>
#include <asmjit/x86.h>

namespace {

struct Store {
    std::size_t   offset;
    std::uint8_t  width;
    std::uint64_t value;
};

std::uint64_t load_le(const char* p, std::size_t n) {
    std::uint64_t v = 0;
    std::memcpy(&v, p, n);
    return v;
}

// Identical plan to src/jit/serializer.cpp.
std::vector<Store> plan_block_write(std::string_view lit) {
    std::vector<Store> out;
    const std::size_t  len = lit.size();
    const char*        p   = lit.data();
    if (len == 0) return out;

    const std::size_t full = (len / 8) * 8;
    for (std::size_t off = 0; off + 8 <= full; off += 8)
        out.push_back({off, 8, load_le(p + off, 8)});

    if (len % 8 != 0) {
        if (len >= 8) {
            const std::size_t off = len - 8;  // overlapping tail
            out.push_back({off, 8, load_le(p + off, 8)});
        } else {
            std::size_t off = 0, rem = len;
            while (rem >= 4) { out.push_back({off, 4, load_le(p + off, 4)}); off += 4; rem -= 4; }
            if    (rem >= 2) { out.push_back({off, 2, load_le(p + off, 2)}); off += 2; rem -= 2; }
            if    (rem >= 1) { out.push_back({off, 1, load_le(p + off, 1)}); }
        }
    }
    return out;
}

void emit_arm64(asmjit::CodeHolder& code, const std::vector<Store>& stores, std::size_t len) {
    namespace a64 = asmjit::a64;
    a64::Assembler a(&code);
    const auto dst = a64::regs::x0;  // size_t fn(char* dst): dst in x0, ret in x0
    for (const Store& s : stores) {
        switch (s.width) {
            case 8:
                a.mov(a64::regs::x9, asmjit::Imm(s.value));
                a.str(a64::regs::x9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
            case 4:
                a.mov(a64::regs::w9, asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                a.str(a64::regs::w9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
            case 2:
                a.mov(a64::regs::w9, asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                a.strh(a64::regs::w9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
            default:
                a.mov(a64::regs::w9, asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                a.strb(a64::regs::w9, a64::ptr(dst, static_cast<int32_t>(s.offset)));
                break;
        }
    }
    a.mov(dst, asmjit::Imm(len));
    a.ret(a64::regs::x30);
}

void emit_x86(asmjit::CodeHolder& code, const std::vector<Store>& stores, std::size_t len) {
    namespace x86 = asmjit::x86;
    x86::Assembler a(&code);
    for (const Store& s : stores) {  // size_t fn(char* dst): dst in rdi, ret in rax
        switch (s.width) {
            case 8:
                a.mov(x86::rdx, asmjit::Imm(s.value));
                a.mov(x86::qword_ptr(x86::rdi, static_cast<int32_t>(s.offset)), x86::rdx);
                break;
            case 4:
                a.mov(x86::dword_ptr(x86::rdi, static_cast<int32_t>(s.offset)),
                      asmjit::Imm(static_cast<std::uint32_t>(s.value)));
                break;
            case 2:
                a.mov(x86::word_ptr(x86::rdi, static_cast<int32_t>(s.offset)),
                      asmjit::Imm(static_cast<std::uint16_t>(s.value)));
                break;
            default:
                a.mov(x86::byte_ptr(x86::rdi, static_cast<int32_t>(s.offset)),
                      asmjit::Imm(static_cast<std::uint8_t>(s.value)));
                break;
        }
    }
    a.mov(x86::rax, asmjit::Imm(len));
    a.ret();
}

void dump(asmjit::Arch arch, const char* title, const std::vector<Store>& stores,
          std::size_t len) {
    asmjit::Environment env;
    env.set_arch(arch);

    asmjit::CodeHolder code;
    code.init(env);

    asmjit::StringLogger logger;
    logger.add_flags(asmjit::FormatFlags::kMachineCode);  // show encoded bytes too
    code.set_logger(&logger);

    if (arch == asmjit::Arch::kAArch64) emit_arm64(code, stores, len);
    else                                emit_x86(code, stores, len);

    std::printf("==================================================================\n");
    std::printf(" %s  (size_t fn(char* dst) -> writes %zu bytes)\n", title, len);
    std::printf("==================================================================\n");
    std::printf("%s\n", logger.data());
    std::printf("  code size: %zu bytes\n\n", code.code_size());
}

}  // namespace

int main() {
    constexpr std::string_view body = "{\"message\":\"Hello, World!\"}";
    const std::vector<Store>    stores = plan_block_write(body);

    std::printf("\nJIT serializer for: %.*s  (%zu bytes)\n",
                static_cast<int>(body.size()), body.data(), body.size());
    std::printf("block-write plan: %zu stores at offsets [", stores.size());
    for (std::size_t i = 0; i < stores.size(); ++i)
        std::printf("%s%zu(%uB)", i ? ", " : "", stores[i].offset, stores[i].width);
    std::printf("]\n\n");

    dump(asmjit::Arch::kAArch64, "ARM64 (AArch64)", stores, body.size());
    dump(asmjit::Arch::kX64,     "x86-64",          stores, body.size());
    return 0;
}
