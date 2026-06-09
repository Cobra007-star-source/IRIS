// =============================================================================
// iris/inspector.hpp  (Phase 4 skeleton)
//
// AST Inspector ("cold-start security inspector")
//
// Responsibilities:
//   - Static topology scan at schema load time
//   - Block pathological rules that would break Fast Path:
//       * $ref cycles (Tarjan SCC size > 1)
//       * unevaluatedProperties
//       * nested depth explosion
//       * known ReDoS-risk patterns
//
// Verdict drives Validator routing: Fast Path when possible, else slow path.
// =============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "iris/common.hpp"

namespace iris {

struct SchemaInspectionInput {
    // Phase 4: replace with JSON Schema tree IR / DAG references.
    // Current stub: fully static.
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

// Run Tarjan SCC + composite rule checks.
InspectionResult inspect(const SchemaInspectionInput& input) noexcept;

}  // namespace iris
