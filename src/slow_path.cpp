// =============================================================================
// slow_path.cpp
// Legacy slow-path entry for CompiledSchema SoA (Inspector force-slow only)
// =============================================================================
#include "iris/slow_path.hpp"

namespace iris {

ValidationReport validate_slow_path(std::string_view /*json*/,
                                    const CompiledSchema& /*schema*/) noexcept {
    ValidationReport r;
    r.code = ValidationError::kNotImplemented;
    return r;
}

}  // namespace iris
