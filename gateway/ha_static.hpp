// =============================================================================
// gateway/ha_static.hpp
//
// HTTPArena `static` profile. Serves 20 files under /static/* with correct
// Content-Type and precompressed variants. On Linux, full responses are baked
// into a sealed memfd and sent via sendfile (Tier 2); otherwise header+writev.
// =============================================================================
#pragma once

#include "iris/http/buffer.hpp"
#include "iris/http/request.hpp"
#include "iris/net/server.hpp"

namespace iris::ha {

// Load every static file found in `dir` into memory, plus any `.br`/`.gz`
// siblings. Returns false if the directory yields no files.
bool load_static(const char* dir) noexcept;

// Handle GET/HEAD /static/<name>. On Linux uses sendfile from a sealed memfd
// (Tier 2); otherwise writes header + writev body (Tier 1). Returns true.
bool handle_static(const iris::http::Request& req, iris::http::Buffer& out,
                   iris::net::AsyncCtx& ctx, int minor_version,
                   bool keep_alive) noexcept;

}  // namespace iris::ha
