// =============================================================================
// examples/hello_iris.cpp
//
// Minimal end-to-end demo:
//   1. Describe schema with FieldSpec
//   2. Compile to CompiledSchema (perfect hash + bitwise type mask)
//   3. Build Validator with dual-engine routing
//   4. Classify valid/invalid JSON samples
// =============================================================================
#include <array>
#include <cstdio>
#include <string_view>

#include "iris/simd_ops.hpp"
#include "iris/validator.hpp"

int main() {
    using namespace iris;

    std::array<FieldSpec, 4> fields = {{
        {"name",   kTypeString,                .required = true,  .min_string_len = 1, .max_string_len = 64},
        {"age",    kTypeInteger,               .required = true,  .min_int = 0,        .max_int = 200},
        {"email",  kTypeString,                .required = false, .min_string_len = 3, .max_string_len = 128},
        {"active", kTypeBoolean,               .required = false},
    }};

    auto built = compile_schema(fields, /*additional_properties=*/false);
    if (!built.ok) {
        std::fprintf(stderr, "compile_schema failed: %s\n", built.diagnostic.c_str());
        return 1;
    }

    Validator v(std::move(built.schema));
    std::printf("[iris] SIMD impl    : %s\n", iris::simd::implementation_name());
    std::printf("[iris] Engine path  : %s\n", engine_path_name(v.path()));
    std::printf("[iris] Required mask: 0x%llx\n",
                static_cast<unsigned long long>(v.schema().required_mask));

    struct Case { const char* tag; std::string_view body; };
    constexpr Case cases[] = {
        {"valid-min",  R"({"name":"Iris","age":3})"},
        {"valid-full", R"({"name":"Iris","age":24,"email":"hi@iris.dev","active":true})"},
        {"valid-ws",   "  {\n  \"name\" : \"\xe7\x88\xb1\xe4\xb8\xbd\xe4\xb8\x9d\",\n  \"age\" : 7\n}  "},
        {"miss-req",   R"({"name":"Iris"})"},
        {"unknown",    R"({"name":"Iris","age":1,"xx":1})"},
        {"bad-type",   R"({"name":"Iris","age":"oops"})"},
        {"too-long",   R"({"name":"123456789012345678901234567890123456789012345678901234567890XXXXXXX","age":1})"},
        {"out-range",  R"({"name":"Iris","age":999})"},
        {"dup",        R"({"name":"a","age":1,"name":"b"})"},
        {"bad-json",   R"({"name":"Iris",,"age":1})"},
    };

    int fails = 0;
    for (auto& c : cases) {
        auto r = v.validate(c.body);
        std::printf("  %-10s | %-18s | offset=%-4u | slot=%-3d | %.*s\n",
                    c.tag,
                    validation_error_name(r.code),
                    r.offset, r.field_slot,
                    static_cast<int>(c.body.size()), c.body.data());
        (void)fails;
    }
    return 0;
}
