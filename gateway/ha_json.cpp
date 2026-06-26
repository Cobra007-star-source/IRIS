// =============================================================================
// gateway/ha_json.cpp
//
// Dataset loader + per-request JSON serializer for the HTTPArena `json` profile.
//
// Strategy: parse the dataset once at startup into per-item byte templates. Each
// template is the item's object text with its closing brace removed, so the hot
// path is: append prefix, append `,"total":<price*quantity*m>}`. price*quantity
// is precomputed per item; only the final multiply and integer-format happen per
// request, honoring the rule that `total` is computed (never cached) per request.
// =============================================================================
#include "ha_json.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace iris::ha {

namespace {

struct Item {
    std::string prefix;   // object text without the trailing '}'
    long        base = 0;  // price * quantity
};

std::vector<Item> g_items;

// Find the matching close brace for the '{' at text[start], skipping braces
// that appear inside JSON strings. Returns the index of the closing '}', or
// std::string::npos on imbalance.
std::size_t match_object(const std::string& text, std::size_t start) noexcept {
    int depth = 0;
    bool in_str = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (in_str) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) return i; }
    }
    return std::string::npos;
}

// Extract the integer value of `"key": <int>` within [obj_begin, obj_end).
// Returns 0 if the key is absent (dataset schema guarantees presence).
long extract_int(const std::string& text, std::size_t begin, std::size_t end,
                 const char* key) noexcept {
    const std::string needle = std::string("\"") + key + "\"";
    std::size_t p = text.find(needle, begin);
    if (p == std::string::npos || p >= end) return 0;
    p += needle.size();
    while (p < end && (text[p] == ' ' || text[p] == '\t' || text[p] == ':' ||
                       text[p] == '\r' || text[p] == '\n')) ++p;
    bool neg = false;
    if (p < end && text[p] == '-') { neg = true; ++p; }
    long v = 0;
    while (p < end && text[p] >= '0' && text[p] <= '9') {
        v = v * 10 + (text[p] - '0');
        ++p;
    }
    return neg ? -v : v;
}

}  // namespace

bool load_dataset(const char* path) noexcept {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (text.empty()) return false;

    g_items.clear();
    std::size_t i = text.find('[');
    if (i == std::string::npos) return false;
    ++i;
    while (i < text.size()) {
        while (i < text.size() && text[i] != '{' && text[i] != ']') ++i;
        if (i >= text.size() || text[i] == ']') break;
        const std::size_t close = match_object(text, i);
        if (close == std::string::npos) return false;
        Item item;
        item.prefix.assign(text, i, close - i);  // includes '{', excludes '}'
        const long price    = extract_int(text, i, close, "price");
        const long quantity = extract_int(text, i, close, "quantity");
        item.base = price * quantity;
        g_items.push_back(std::move(item));
        i = close + 1;
    }
    return !g_items.empty();
}

std::size_t dataset_size() noexcept { return g_items.size(); }

void serialize_json(iris::http::Buffer& out, std::size_t count,
                    long multiplier) noexcept {
    if (count > g_items.size()) count = g_items.size();
    out.append("{\"items\":[");
    for (std::size_t k = 0; k < count; ++k) {
        if (k) out.append(',');
        const Item& it = g_items[k];
        out.append(std::string_view(it.prefix));
        out.append(",\"total\":");
        const long total = it.base * multiplier;
        if (total < 0) {
            out.append('-');
            out.append_uint(static_cast<std::size_t>(-total));
        } else {
            out.append_uint(static_cast<std::size_t>(total));
        }
        out.append('}');
    }
    out.append("],\"count\":");
    out.append_uint(count);
    out.append('}');
}

}  // namespace iris::ha
