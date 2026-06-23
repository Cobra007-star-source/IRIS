#include "wfb_static.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "iris/http/date.hpp"

namespace iris::wfb {

namespace {

struct FileEntry {
    const char*       name = nullptr;
    std::vector<char> data;
    char              etag[32] = {};
};

FileEntry g_15k;
FileEntry g_1m;

bool read_file(const char* dir, const char* name, FileEntry& out) {
    std::string path = dir;
    if (!path.empty() && path.back() != '/') path += '/';
    path += name;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const auto sz = in.tellg();
    if (sz <= 0) return false;
    out.data.resize(static_cast<std::size_t>(sz));
    in.seekg(0, std::ios::beg);
    in.read(out.data.data(), sz);
    out.name = name;
    std::snprintf(out.etag, sizeof(out.etag), "\"%zu-%s\"", out.data.size(), name);
    return true;
}

const FileEntry* find_file(std::string_view path) noexcept {
    if (path == "/files/15kb.bin") return &g_15k;
    if (path == "/files/1mb.bin") return &g_1m;
    return nullptr;
}

void write_static(iris::http::Buffer& out, int status, const char* reason,
                  std::size_t content_length, const char* etag, int minor_version,
                  bool keep_alive, bool with_body, std::string_view body,
                  const char* content_range = nullptr) noexcept {
    out.append("HTTP/1.1 ");
    out.append_uint(static_cast<std::size_t>(status));
    out.append(' ');
    out.append(reason);
    out.append("\r\nServer: iris\r\nDate: ");
    out.append(iris::http::current_date());
    out.append("\r\nContent-Type: application/octet-stream\r\nContent-Length: ");
    out.append_uint(content_length);
    out.append("\r\nETag: ");
    out.append(etag);
    if (content_range) {
        out.append("\r\nContent-Range: ");
        out.append(content_range);
    }
    if (!keep_alive) {
        out.append("\r\nConnection: close");
    } else if (minor_version == 0) {
        out.append("\r\nConnection: keep-alive");
    }
    out.append("\r\n\r\n");
    if (with_body && !body.empty()) out.append(body);
}

bool parse_range(std::string_view hdr, std::size_t file_size, std::size_t& start,
                 std::size_t& end) noexcept {
    if (hdr.size() < 6 || hdr.substr(0, 6) != "bytes=") return false;
    const std::size_t dash = hdr.find('-', 6);
    if (dash == std::string_view::npos) return false;
    auto parse_u = [](std::string_view s, std::size_t& v) {
        v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<std::size_t>(c - '0');
        }
        return true;
    };
    if (!parse_u(hdr.substr(6, dash - 6), start)) return false;
    if (!parse_u(hdr.substr(dash + 1), end)) return false;
    if (start > end || end >= file_size) return false;
    return true;
}

inline bool etag_matches(std::string_view inm, const char* etag) noexcept {
    while (!inm.empty() && (inm.front() == ' ' || inm.front() == '\t')) {
        inm.remove_prefix(1);
    }
    const std::size_t elen = std::strlen(etag);
    return inm.size() >= elen && inm.substr(0, elen) == etag;
}

}  // namespace

bool load_static_files(const char* data_dir) noexcept {
    const char* dir = data_dir ? data_dir : "benchmarks_data";
    return read_file(dir, "15kb.bin", g_15k) && read_file(dir, "1mb.bin", g_1m);
}

bool handle_static(const iris::http::Request& req, iris::http::Buffer& out,
                   int minor_version, bool keep_alive) noexcept {
    const FileEntry* fe = find_file(req.path);
    if (!fe || fe->data.empty()) return false;

    const bool is_head = req.method == "HEAD";
    const bool is_get  = req.method == "GET";
    if (!is_get && !is_head) return false;

    if (!req.if_none_match.empty() && etag_matches(req.if_none_match, fe->etag)) {
        write_static(out, 304, "Not Modified", 0, fe->etag, minor_version,
                     keep_alive, false, {});
        return true;
    }

    std::size_t off = 0;
    std::size_t len = fe->data.size();

    if (is_get && !req.range.empty()) {
        std::size_t start = 0, end = 0;
        if (parse_range(req.range, fe->data.size(), start, end)) {
            off = start;
            len = end - start + 1;
            char cr[64];
            std::snprintf(cr, sizeof(cr), "bytes %zu-%zu/%zu", start, end,
                          fe->data.size());
            write_static(out, 206, "Partial Content", len, fe->etag, minor_version,
                         keep_alive, !is_head,
                         std::string_view(fe->data.data() + off, len), cr);
            return true;
        }
    }

    write_static(out, 200, "OK", len, fe->etag, minor_version, keep_alive, !is_head,
                 std::string_view(fe->data.data() + off, len));
    return true;
}

}  // namespace iris::wfb
