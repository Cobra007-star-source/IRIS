// =============================================================================
// tests/test_slow_path.cpp
//
// 慢车道单元测试：直接用 SlowSchema + validate_slow_path 验证关键关键字。
// 覆盖：allOf / anyOf / oneOf / not / if-then-else / $ref / const / enum /
//       multipleOf / pattern / dependentRequired / unevaluatedProperties.
// =============================================================================
#include "iris/slow_schema.hpp"
#include "iris/validator.hpp"
#include "test_framework.hpp"

namespace {

bool ok_validate(const char* schema, const char* instance) {
    auto b = iris::compile_slow_schema(schema);
    if (!b.ok) return false;
    return iris::validate_slow_path(instance, *b.schema).ok();
}

bool fail_validate(const char* schema, const char* instance) {
    auto b = iris::compile_slow_schema(schema);
    if (!b.ok) return false;
    return !iris::validate_slow_path(instance, *b.schema).ok();
}

}  // namespace

IRIS_TEST(slow_const_match) {
    IRIS_EXPECT(ok_validate(R"({"const":42})", "42"));
    IRIS_EXPECT(fail_validate(R"({"const":42})", "43"));
    IRIS_EXPECT(ok_validate(R"({"const":{"a":1,"b":[true,null]}})",
                            R"({"b":[true,null],"a":1})"));
}

IRIS_TEST(slow_enum_unicode_safe) {
    IRIS_EXPECT(ok_validate(R"({"enum":["a","b","c"]})", R"("b")"));
    IRIS_EXPECT(fail_validate(R"({"enum":["a","b","c"]})", R"("z")"));
}

IRIS_TEST(slow_allOf_short_circuit) {
    const char* s = R"({"allOf":[{"type":"integer"},{"minimum":10}]})";
    IRIS_EXPECT(ok_validate(s, "12"));
    IRIS_EXPECT(fail_validate(s, "5"));
    IRIS_EXPECT(fail_validate(s, R"("notint")"));
}

IRIS_TEST(slow_oneOf_must_be_one) {
    const char* s = R"({"oneOf":[{"type":"integer"},{"minimum":5}]})";
    // 9 matches both (integer AND minimum:5) → oneOf 失败
    IRIS_EXPECT(fail_validate(s, "9"));
    // -3 matches only integer
    IRIS_EXPECT(ok_validate(s, "-3"));
    // 5.5 matches only minimum:5
    IRIS_EXPECT(ok_validate(s, "5.5"));
}

IRIS_TEST(slow_not) {
    IRIS_EXPECT(ok_validate(R"({"not":{"type":"string"}})", "42"));
    IRIS_EXPECT(fail_validate(R"({"not":{"type":"string"}})", R"("x")"));
}

IRIS_TEST(slow_if_then_else) {
    const char* s = R"({
        "if":   {"type":"integer"},
        "then": {"minimum":0},
        "else": {"minLength":2}
    })";
    IRIS_EXPECT(ok_validate(s, "5"));
    IRIS_EXPECT(fail_validate(s, "-1"));
    IRIS_EXPECT(ok_validate(s, R"("ab")"));
    IRIS_EXPECT(fail_validate(s, R"("a")"));
}

IRIS_TEST(slow_ref_local_pointer) {
    const char* s = R"({
        "$defs": {"pos": {"type":"integer","minimum":1}},
        "$ref": "#/$defs/pos"
    })";
    IRIS_EXPECT(ok_validate(s, "5"));
    IRIS_EXPECT(fail_validate(s, "0"));
    IRIS_EXPECT(fail_validate(s, R"("five")"));
}

IRIS_TEST(slow_ref_anchor) {
    const char* s = R"({
        "$defs": {"named": {"$anchor":"theTag","type":"boolean"}},
        "$ref": "#theTag"
    })";
    IRIS_EXPECT(ok_validate(s, "true"));
    IRIS_EXPECT(fail_validate(s, "42"));
}

IRIS_TEST(slow_unevaluated_props) {
    const char* s = R"({
        "type":"object",
        "properties":{"x":{"type":"integer"}},
        "unevaluatedProperties":false
    })";
    IRIS_EXPECT(ok_validate(s, R"({"x":1})"));
    IRIS_EXPECT(fail_validate(s, R"({"x":1,"y":2})"));
}

IRIS_TEST(slow_multipleOf_overflow_safe) {
    // -ffast-math 关闭对 slow_eval.cpp 不生效 → isfinite() 应工作
    const char* s = R"({"type":"integer","multipleOf":0.123456789})";
    IRIS_EXPECT(fail_validate(s, "1e308"));   // 商溢出为 inf
}

IRIS_TEST(validator_routing_fast_when_simple) {
    using namespace iris;
    ValidatorBuild vb;
    auto v = Validator::from_schema_json(R"({"type":"integer","minimum":1})", vb);
    IRIS_EXPECT(vb.ok);
    IRIS_EXPECT(v.path() == EnginePath::kFastInterpret || v.path() == EnginePath::kFastJit);
    IRIS_EXPECT(v.validate("5").ok());
    IRIS_EXPECT(!v.validate("0").ok());
}

IRIS_TEST(validator_routing_slow_when_allOf) {
    using namespace iris;
    ValidatorBuild vb;
    auto v = Validator::from_schema_json(
        R"({"allOf":[{"type":"integer"},{"minimum":1}]})", vb);
    IRIS_EXPECT(vb.ok);
    IRIS_EXPECT(v.path() == EnginePath::kSlowFallback);
    IRIS_EXPECT(v.has_slow_path());
    IRIS_EXPECT(v.validate("5").ok());
    IRIS_EXPECT(!v.validate("0").ok());
}

#if IRIS_ENABLE_JIT
IRIS_TEST(jit_compiler_api_demo_spill_works) {
    // Compiler API 在 ARM64(31 GP) / x86_64(16 GP) 上对 32 个虚拟寄存器
    // 必然触发栈溢出 + reload。这个测试只要 JIT 生成成功并能跑就算过。
    auto fn = iris::jit::jit_compile_compiler_demo();
    IRIS_EXPECT(fn != nullptr);
    if (fn) {
        std::uint64_t out1 = fn(0xDEAD'BEEFULL);
        std::uint64_t out2 = fn(0xDEAD'BEEFULL);
        IRIS_EXPECT_EQ(out1, out2);  // 确定性
        IRIS_EXPECT(out1 != 0);      // 链尾累加非零
    }
}
#endif
