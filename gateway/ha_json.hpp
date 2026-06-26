// =============================================================================
// gateway/ha_json.hpp
//
// HTTPArena `json` profile. Loads /data/dataset.json once at startup, then
// serializes GET /json/{count}?m={multiplier} per request: the first `count`
// items, each with a computed total = price * quantity * multiplier.
// =============================================================================
#pragma once

#include <cstddef>

#include "iris/http/buffer.hpp"

namespace iris::ha {

// Parse the dataset file into per-item templates. Returns false if the file is
// missing or malformed.
bool load_dataset(const char* path) noexcept;

// Number of items available in the loaded dataset.
std::size_t dataset_size() noexcept;

// Serialize the JSON array body for `count` items (clamped to the dataset size)
// with the given multiplier into `out`.
void serialize_json(iris::http::Buffer& out, std::size_t count,
                    long multiplier) noexcept;

}  // namespace iris::ha
