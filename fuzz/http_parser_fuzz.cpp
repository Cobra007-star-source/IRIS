// =============================================================================
// fuzz/http_parser_fuzz.cpp
//
// libFuzzer harness for iris::http::parse_request. Feeds arbitrary bytes to the
// parser and asserts the documented contract: parse_request never reads out of
// bounds, never crashes, and reports `consumed <= len` on success. Build with
// -DIRIS_BUILD_FUZZER=ON and run e.g.:
//
//   ./iris_http_fuzz -max_total_time=60 fuzz-corpus/
// =============================================================================
#include <cstddef>
#include <cstdint>

#include "iris/http/parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    iris::http::Request req;
    auto r = iris::http::parse_request(reinterpret_cast<const char*>(data), size, req);
    if (r.status == iris::http::ParseStatus::kOk) {
        // Contract: a successful parse consumes no more than the input span, and
        // the parsed views must lie within that span.
        if (r.consumed > size) __builtin_trap();
        if (!req.method.empty()) {
            const char* base = reinterpret_cast<const char*>(data);
            if (req.method.data() < base || req.method.data() + req.method.size() > base + size)
                __builtin_trap();
        }
    }
    return 0;
}
