// =============================================================================
// iris/http/date.hpp
//
// Global HTTP Date string, refreshed once per second by a dedicated timer
// thread. Worker threads read it via an atomic load + a ~29-byte memcpy, so the
// hot path never calls time()/gmtime() (those would trap into the kernel and
// stall the pipeline at millions of requests/second).
// =============================================================================
#pragma once

#include <string_view>

namespace iris::http {

// Idempotent: starts the background 1 Hz refresh thread on first call and
// formats the initial value synchronously so the first response is valid.
void start_date_clock() noexcept;

// Current IMF-fixdate value, e.g. "Sun, 06 Nov 1994 08:49:37 GMT" (29 bytes).
// Cheap and thread-safe.
[[nodiscard]] std::string_view current_date() noexcept;

}  // namespace iris::http
