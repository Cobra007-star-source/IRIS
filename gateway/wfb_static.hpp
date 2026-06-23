#pragma once

#include <cstddef>

#include "iris/http/buffer.hpp"
#include "iris/http/request.hpp"

namespace iris::wfb {

// Load 15kb.bin and 1mb.bin from DATA_DIR at startup.
bool load_static_files(const char* data_dir) noexcept;

// Handle GET/HEAD /files/*.bin (Range, ETag, 304). Returns true if handled.
bool handle_static(const iris::http::Request& req, iris::http::Buffer& out,
                   int minor_version, bool keep_alive) noexcept;

}  // namespace iris::wfb
