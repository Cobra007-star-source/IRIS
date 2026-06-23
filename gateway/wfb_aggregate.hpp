#pragma once

#include <string_view>

#include "iris/http/buffer.hpp"

namespace iris::wfb {

// WFB POST /json/aggregate: single-pass aggregation without heap allocation.
void json_aggregate(std::string_view body, iris::http::Buffer& out) noexcept;

}  // namespace iris::wfb
