// =============================================================================
// iris/slow_schema.hpp
//
// Slow-path schema representation: full JSON Schema AST plus precompiled indexes
// ($ref / $defs table + regex cache).
//
// Design principles
// ---------
//   1. **Zero information loss**: JSON Schema parsed to JsonValue keeps the full tree;
//      keywords Fast Path cannot express ($ref / allOf / pattern / ...) run on this AST.
//   2. **Pre-compile heavy objects**: $ref table and RE2 regex built once at schema compile;
//      hot path only does pointer lookup, no string parsing.
//   3. **Zero-copy hand-off**: shares the same JSON input bytes with Fast Path.
//      Slow Path re-parses instances via JsonReader (tree access required), but schema
//      side has no second copy.
//
// Public API
// ----------------
//     auto built = iris::compile_slow_schema(R"({"allOf":[...]})");
//     if (!built.ok) { ... handle error ... }
//     auto r = iris::validate_slow_path(R"({"x":1})", *built.schema);
// =============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "iris/json_reader.hpp"
#include "iris/parser.hpp"

namespace iris {

// Opaque compiled regex; implementation in src/slow_schema.cpp (RE2 or std::regex).
struct CompiledRegex;
struct CompiledRegexDeleter {
    void operator()(CompiledRegex* p) const noexcept;
};

// Compile artifact. JsonValue owns all subtrees; resources/regex_cache hold references only.
// SlowSchema is move-only.
//
// Cross-document $ref (http(s)://) via remote_roots + add_remote_document:
//   1. After main schema compile, call add_remote_document(uri, json).
//   2. Each remote doc owns a JsonValue; all $id / $anchor / JSON-pointer forms registered.
//   3. At eval time $ref resolves to absolute URI and looks up resources table.
struct SlowSchema {
    SlowSchema();
    ~SlowSchema();
    SlowSchema(SlowSchema&&) noexcept;
    SlowSchema& operator=(SlowSchema&&) noexcept;

    // Original schema document (owned)
    JsonValue root;

    // Remote documents injected via add_remote_document(), owned. slow_eval accesses via resources.
    std::vector<std::unique_ptr<JsonValue>> remote_roots;

    // URI -> subschema node lookup. Keys include:
    //   "<absolute_uri>"              : whole subschema via $id
    //   "<absolute_uri>#<anchor>"     : via $anchor / $dynamicAnchor
    //   "<absolute_uri>#/path/.../"   : via JSON Pointer (each node)
    //   ""                            : primary document root (fallback)
    //   "#/path/..."                  : primary document JSON Pointer (no $id)
    //   "anchor:<name>"               : legacy same-document anchor key
    std::unordered_map<std::string, const JsonValue*> resources;

    // Legacy same-document refs (compat; overlaps resources).
    std::unordered_map<std::string, const JsonValue*> refs;

    // Each subschema object node -> effective base URI (nearest ancestor $id).
    // Zero-copy base lookup for $ref / $dynamicRef RFC 3986 resolution.
    std::unordered_map<const JsonValue*, std::string> node_base;

    // Primary document base URI (root.$id, or empty).
    std::string primary_base;

    // Regex cache: each string subnode with "pattern" -> compiled regex.
    std::unordered_map<const JsonValue*,
                       std::unique_ptr<CompiledRegex, CompiledRegexDeleter>> regex_cache;

    // Max recursion depth (guard infinite $ref expansion)
    int max_eval_depth = 128;

    // Whether validation vocabulary is disabled (from $schema metaschema $vocabulary).
    // -1=unchecked; 0=enabled; 1=disabled. Lazy on first validate (remote docs may inject later).
    mutable int disable_validation_vocab = -1;
};

struct SlowSchemaBuildResult {
    bool                          ok = false;
    std::unique_ptr<SlowSchema>   schema;
    std::string                   diagnostic;
};

// Compile JSON Schema text to SlowSchema:
//   1. Parse to JsonValue
//   2. Recursively collect $defs / named $ref nodes
//   3. Compile all patterns to RE2
//   4. Unresolvable refs (e.g. http:// cross-document) -> ok=false
[[nodiscard]] SlowSchemaBuildResult compile_slow_schema(std::string_view schema_json);

// Main entry: slow-path validation.
//   - json_instance: raw JSON bytes, zero-copy; parsed to JsonValue tree internally
//   - schema: compiled SlowSchema
ValidationReport validate_slow_path(std::string_view json_instance,
                                    const SlowSchema& schema) noexcept;

// Inject external document for $ref resolution. uri must be absolute (with scheme);
// json is document literal. Returns false on parse/URI failure; no throw.
// Re-injecting same uri overwrites prior entry.
bool slow_schema_add_remote(SlowSchema& s, std::string uri, std::string_view json) noexcept;

// Whether RE2 compile support is available (CI/diagnostics only).
[[nodiscard]] bool slow_path_has_re2() noexcept;

}  // namespace iris
