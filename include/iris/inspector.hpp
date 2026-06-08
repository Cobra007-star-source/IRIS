// =============================================================================
// iris/inspector.hpp  (Phase 4 骨架)
//
// AST Inspector ("冷启动安检员")
//
// 职责：
//   - 在 schema 加载阶段做静态拓扑扫描
//   - 拦截会引爆 Fast Path 的“变态规则”：
//       * $ref 环路 (Tarjan SCC > 1 即触发)
//       * unevaluatedProperties
//       * 嵌套深度爆炸
//       * 已知 ReDoS 风险的 pattern
//
// 决策结果驱动 Validator 路由：能上 Fast Path 上 Fast Path，
// 否则进入慢车道。
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/common.hpp"

namespace iris {

struct SchemaInspectionInput {
    // Phase 4 真正接入 JSON Schema 树时，这里改为 IR / DAG 引用。
    // 当前用 stub：完全静态。
    std::vector<std::vector<std::uint32_t>> ref_graph;
    bool has_unevaluated_properties = false;
    std::uint32_t max_depth = 0;
};

enum class InspectionVerdict : std::uint8_t {
    kFastPathOk = 0,
    kForceSlowPath,
};

struct InspectionResult {
    InspectionVerdict verdict = InspectionVerdict::kFastPathOk;
    std::string       reason;
};

// 执行 Tarjan SCC + 复合规则判定。
InspectionResult inspect(const SchemaInspectionInput& input) noexcept;

}  // namespace iris
