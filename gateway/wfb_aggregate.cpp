#include "wfb_aggregate.hpp"

#include <cstdint>
#include <cstring>

namespace iris::wfb {

namespace {

constexpr const char* kCountries[]  = {"US", "DE", "FR", "UK", "JP"};
constexpr const char* kCategories[] = {"Electronics", "Books", "Clothing", "Home"};

inline void skip_ws(const char*& p, const char* end) noexcept {
    while (p < end) {
        const char c = *p;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
        ++p;
    }
}

// Parse a non-negative integer, advancing p. A leading '-' is consumed but the
// magnitude is still accumulated (amounts/quantities in this workload are >= 0).
inline long long parse_int(const char*& p, const char* end) noexcept {
    if (p < end && *p == '-') ++p;
    long long v = 0;
    while (p < end) {
        const unsigned d = static_cast<unsigned>(*p) - '0';
        if (d > 9) break;
        v = v * 10 + static_cast<long long>(d);
        ++p;
    }
    return v;
}

// Read a JSON string token. On entry p must point at the opening quote. Sets
// (s, n) to the raw inner bytes and advances p past the closing quote.
inline bool read_string(const char*& p, const char* end, const char*& s,
                        std::size_t& n) noexcept {
    if (p >= end || *p != '"') return false;
    ++p;
    s = p;
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) p += 2;
        else ++p;
    }
    if (p >= end) return false;
    n = static_cast<std::size_t>(p - s);
    ++p;
    return true;
}

// Country codes are two ASCII bytes; pack and switch (no memcmp / strlen).
inline int country_index(const char* s, std::size_t n) noexcept {
    if (n != 2) return -1;
    const std::uint16_t k =
        static_cast<std::uint16_t>((static_cast<unsigned char>(s[0]) << 8) |
                                   static_cast<unsigned char>(s[1]));
    switch (k) {
        case ('U' << 8) | 'S': return 0;  // US
        case ('D' << 8) | 'E': return 1;  // DE
        case ('F' << 8) | 'R': return 2;  // FR
        case ('U' << 8) | 'K': return 3;  // UK
        case ('J' << 8) | 'P': return 4;  // JP
        default:               return -1;
    }
}

// The four categories have unique lengths, so length alone selects the bucket;
// a single confirming memcmp guards against same-length impostors.
inline int category_index(const char* s, std::size_t n) noexcept {
    switch (n) {
        case 4:  return std::memcmp(s, "Home", 4) == 0 ? 3 : -1;
        case 5:  return std::memcmp(s, "Books", 5) == 0 ? 1 : -1;
        case 8:  return std::memcmp(s, "Clothing", 8) == 0 ? 2 : -1;
        case 11: return std::memcmp(s, "Electronics", 11) == 0 ? 0 : -1;
        default: return -1;
    }
}

// Advance p past one JSON value (object, array, string, number, literal).
void skip_value(const char*& p, const char* end) noexcept {
    skip_ws(p, end);
    if (p >= end) return;
    const char c = *p;
    if (c == '"') {
        const char* s;
        std::size_t n;
        read_string(p, end, s, n);
    } else if (c == '{' || c == '[') {
        const char open  = c;
        const char close = (c == '{') ? '}' : ']';
        int        depth = 0;
        while (p < end) {
            const char ch = *p;
            if (ch == '"') {  // strings may contain braces/brackets
                const char* s;
                std::size_t n;
                read_string(p, end, s, n);
                continue;
            }
            if (ch == open) ++depth;
            else if (ch == close && --depth == 0) { ++p; return; }
            ++p;
        }
    } else {
        // number / true / false / null
        while (p < end) {
            const char ch = *p;
            if (ch == ',' || ch == '}' || ch == ']') break;
            ++p;
        }
    }
}

// Parse one item object in a single pass, accumulating quantity-per-category
// into `litem`. On entry p points at '{'; on return p is past the matching '}'.
inline void parse_item(const char*& p, const char* end,
                       long long* litem) noexcept {
    ++p;  // consume '{'
    long long qty = 0;
    int       cat = -1;
    for (;;) {
        skip_ws(p, end);
        if (p >= end || *p == '}') { if (p < end) ++p; break; }
        if (*p == ',') { ++p; continue; }
        if (*p != '"') { ++p; continue; }

        const char* key;
        std::size_t klen;
        read_string(p, end, key, klen);
        skip_ws(p, end);
        if (p < end && *p == ':') ++p;
        skip_ws(p, end);

        // quantity(8,'q'), category(8,'c'); price(5) and others skipped.
        if (klen == 8 && key[0] == 'q') {
            qty = parse_int(p, end);
        } else if (klen == 8 && key[0] == 'c') {
            const char* vs;
            std::size_t vn;
            if (read_string(p, end, vs, vn)) cat = category_index(vs, vn);
            else skip_value(p, end);
        } else {
            skip_value(p, end);
        }
    }
    if (cat >= 0 && qty > 0) litem[cat] += qty;
}

// Parse the items array in one pass. On entry p points at '['; on return p is
// past the matching ']'.
inline void parse_items(const char*& p, const char* end,
                        long long* litem) noexcept {
    ++p;  // consume '['
    for (;;) {
        skip_ws(p, end);
        if (p >= end || *p == ']') { if (p < end) ++p; break; }
        if (*p == ',') { ++p; continue; }
        if (*p == '{') parse_item(p, end, litem);
        else skip_value(p, end);
    }
}

// Parse one order object in a single pass. On entry p points at '{'; on return
// p is past the matching '}'. Updates the global accumulators when completed.
void parse_order(const char*& p, const char* end, int& processed,
                 long long* results, long long* cat_stats) noexcept {
    ++p;  // consume '{'
    bool      completed = false;
    long long amount    = 0;
    int       country   = -1;
    long long litem[4]  = {};

    for (;;) {
        skip_ws(p, end);
        if (p >= end || *p == '}') { if (p < end) ++p; break; }
        if (*p == ',') { ++p; continue; }
        if (*p != '"') { ++p; continue; }

        const char* key;
        std::size_t klen;
        read_string(p, end, key, klen);
        skip_ws(p, end);
        if (p < end && *p == ':') ++p;
        skip_ws(p, end);

        // status(6,'s'), amount(6,'a'), country(7), items(5).
        if (klen == 6 && key[0] == 's') {
            const char* vs;
            std::size_t vn;
            if (read_string(p, end, vs, vn)) {
                completed = (vn == 9 && std::memcmp(vs, "completed", 9) == 0);
            } else {
                skip_value(p, end);
            }
        } else if (klen == 6 && key[0] == 'a') {
            amount = parse_int(p, end);
        } else if (klen == 7 && key[0] == 'c') {
            const char* vs;
            std::size_t vn;
            if (read_string(p, end, vs, vn)) country = country_index(vs, vn);
            else skip_value(p, end);
        } else if (klen == 5 && key[0] == 'i') {
            skip_ws(p, end);
            if (p < end && *p == '[') parse_items(p, end, litem);
            else skip_value(p, end);
        } else {
            skip_value(p, end);
        }
    }

    if (!completed) return;
    ++processed;
    if (country >= 0) results[country] += amount;
    for (int i = 0; i < 4; ++i) cat_stats[i] += litem[i];
}

}  // namespace

void json_aggregate(std::string_view body, iris::http::Buffer& out) noexcept {
    const char* p   = body.data();
    const char* end = p + body.size();

    int       processed   = 0;
    long long results[5]   = {};
    long long cat_stats[4] = {};

    skip_ws(p, end);
    if (p < end && *p == '[') ++p;

    for (;;) {
        skip_ws(p, end);
        if (p >= end || *p == ']') break;
        if (*p == ',') { ++p; continue; }
        if (*p != '{') break;
        parse_order(p, end, processed, results, cat_stats);
    }

    out.append("{\"processedOrders\":");
    out.append_uint(static_cast<std::size_t>(processed));
    out.append(",\"results\":{");
    for (int i = 0; i < 5; ++i) {
        if (i) out.append(',');
        out.append('"');
        out.append(kCountries[i]);
        out.append("\":");
        out.append_uint(static_cast<std::size_t>(results[i]));
    }
    out.append("},\"categoryStats\":{");
    for (int i = 0; i < 4; ++i) {
        if (i) out.append(',');
        out.append('"');
        out.append(kCategories[i]);
        out.append("\":");
        out.append_uint(static_cast<std::size_t>(cat_stats[i]));
    }
    out.append("}}");
}

}  // namespace iris::wfb
