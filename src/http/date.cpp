// =============================================================================
// src/http/date.cpp
//
// Double-buffered, lock-free HTTP Date string. A detached timer thread formats
// the current time into the inactive buffer twice a second and flips an atomic
// index; readers do an acquire-load of the index and return a view of that
// buffer. The 29-byte IMF-fixdate length is fixed, so no length is published.
// =============================================================================
#include "iris/http/date.hpp"

#include <atomic>
#include <chrono>
#include <ctime>
#include <mutex>
#include <thread>

namespace iris::http {

namespace {

constexpr int kDateLen = 29;  // "Sun, 06 Nov 1994 08:49:37 GMT"

char                 g_buf[2][32];
std::atomic<int>     g_idx{0};
std::once_flag       g_once;

void format_into(char* dst) noexcept {
    std::time_t t = std::time(nullptr);
    std::tm     tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    // C locale yields the English day/month abbreviations TFB expects.
    std::strftime(dst, 32, "%a, %d %b %Y %H:%M:%S GMT", &tm);
}

void clock_thread() noexcept {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        int next = 1 - g_idx.load(std::memory_order_relaxed);
        format_into(g_buf[next]);
        g_idx.store(next, std::memory_order_release);
    }
}

}  // namespace

void start_date_clock() noexcept {
    std::call_once(g_once, [] {
        format_into(g_buf[0]);
        format_into(g_buf[1]);
        g_idx.store(0, std::memory_order_release);
        std::thread(clock_thread).detach();
    });
}

std::string_view current_date() noexcept {
    int i = g_idx.load(std::memory_order_acquire);
    return std::string_view(g_buf[i], kDateLen);
}

}  // namespace iris::http
