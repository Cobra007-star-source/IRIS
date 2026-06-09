// =============================================================================
// iris/slow_path.hpp  (Phase 4 skeleton)
//
// Slow path: schemas blocked by Inspector use a general interpreter to ensure
// Bowtie 100% compliance.
//
// Current implementation: placeholder interpreter API that accepts everything
// as-is; only shapes the API surface.
//
// TODO(phase-4): wire up QuickJS or a custom AST walker.
// =============================================================================
#pragma once

#include <cstdint>
#include <string_view>

#include "iris/parser.hpp"
#include "iris/schema.hpp"

namespace iris {

ValidationReport validate_slow_path(std::string_view json,
                                    const CompiledSchema& schema) noexcept;

}  // namespace iris
