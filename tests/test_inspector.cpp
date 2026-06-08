// =============================================================================
// tests/test_inspector.cpp
// =============================================================================
#include "iris/inspector.hpp"
#include "iris/validator.hpp"
#include "test_framework.hpp"

IRIS_TEST(inspector_self_loop_blocks_fast_path) {
    iris::SchemaInspectionInput in;
    in.ref_graph = {{0}};  // 0 -> 0
    auto r = iris::inspect(in);
    IRIS_EXPECT(r.verdict == iris::InspectionVerdict::kForceSlowPath);
}

IRIS_TEST(inspector_cycle_blocks_fast_path) {
    iris::SchemaInspectionInput in;
    in.ref_graph = {{1}, {2}, {0}};  // 0->1->2->0
    auto r = iris::inspect(in);
    IRIS_EXPECT(r.verdict == iris::InspectionVerdict::kForceSlowPath);
}

IRIS_TEST(inspector_dag_ok) {
    iris::SchemaInspectionInput in;
    in.ref_graph = {{1, 2}, {2}, {}};
    auto r = iris::inspect(in);
    IRIS_EXPECT(r.verdict == iris::InspectionVerdict::kFastPathOk);
}

IRIS_TEST(inspector_unevaluated_props) {
    iris::SchemaInspectionInput in;
    in.has_unevaluated_properties = true;
    auto r = iris::inspect(in);
    IRIS_EXPECT(r.verdict == iris::InspectionVerdict::kForceSlowPath);
}

// Inspector 拦截后慢车道必须显式失败，绝不能悄悄回退 Fast Path。
// 这是 v0.2 之前的隐性 bug：unsupported schema 被默默近似处理。
IRIS_TEST(inspector_forced_slow_path_returns_not_implemented) {
    using namespace iris;
    auto built = compile_schema_from_json(R"({"type":"object","properties":{"x":{"type":"integer"}}})");
    IRIS_EXPECT(built.ok);

    SchemaInspectionInput in;
    in.ref_graph = {{0}};   // 故意构造 $ref 自环让 Inspector 强制慢车道
    Validator v(std::move(built.schema), in);

    IRIS_EXPECT(v.path() == EnginePath::kSlowFallback);
    auto r = v.validate(R"({"x":1})");
    IRIS_EXPECT(!r.ok());
    IRIS_EXPECT_EQ(r.code, ValidationError::kNotImplemented);
}
