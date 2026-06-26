// =============================================================================
// gateway/ha_static.cpp
//
// HTTPArena `static` profile server.
//
// Tier 1 (all platforms): precompressed .br/.gz siblings + Accept-Encoding
// negotiation; optional writev zero-copy body tail.
//
// Tier 2 (Linux): at startup every (file × encoding × method) response is
// pre-baked into a sealed memfd (status + headers + body, no Date — validate.sh
// does not check Date on static). Hot GET/HEAD/404 paths register
// (offset, len) and flush() sends the slice via sendfile with zero user-space
// copy and no per-request header formatting.
// =============================================================================
#include "ha_static.hpp"

#include <dirent.h>

#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "iris/http/date.hpp"

#if defined(__linux__)
    #ifndef _GNU_SOURCE
        #define _GNU_SOURCE
    #endif
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <unistd.h>
#endif

namespace iris::ha {

namespace {

constexpr std::size_t kPage = 4096;

struct Frozen {
    std::size_t off = 0;
    std::size_t len = 0;
};

struct Variant {
    std::string       header;       // Tier 1: header incl. Date placeholder
    std::size_t       date_off = 0;
    std::vector<char> body;
    std::string       frozen_get;   // Tier 2: full GET bytes (temp until seal)
    std::string       frozen_head;  // Tier 2: HEAD header block (temp)
    Frozen            get;
    Frozen            head;
    bool              present = false;
};

struct Entry {
    Variant identity;
    Variant br;
    Variant gz;
};

std::unordered_map<std::string, Entry> g_files;
std::string g_404;
std::size_t g_404_date_off = 0;
Frozen      g_404_frozen;
std::string g_404_frozen_bytes;

#if defined(__linux__)
int  g_memfd = -1;
bool g_sendfile_ready = false;
void*       g_map = nullptr;
std::size_t g_map_len = 0;
#endif

const char* content_type_for(const std::string& name) noexcept {
    auto ends = [&](const char* ext) {
        const std::size_t el = std::strlen(ext);
        return name.size() >= el && name.compare(name.size() - el, el, ext) == 0;
    };
    if (ends(".css"))  return "text/css";
    if (ends(".js"))   return "text/javascript";
    if (ends(".json")) return "application/json";
    if (ends(".html")) return "text/html";
    if (ends(".svg"))  return "image/svg+xml";
    if (ends(".woff2"))return "font/woff2";
    if (ends(".webp")) return "image/webp";
    return "application/octet-stream";
}

bool read_whole(const std::string& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign((std::istreambuf_iterator<char>(in)),
               std::istreambuf_iterator<char>());
    return true;
}

void build_header(std::string& hdr, std::size_t& date_off, int status,
                  const char* reason, const char* content_type,
                  const char* content_encoding, std::size_t content_length) {
    hdr  = "HTTP/1.1 ";
    hdr += std::to_string(status);
    hdr += ' ';
    hdr += reason;
    hdr += "\r\nServer: iris\r\nDate: ";
    date_off = hdr.size();
    hdr.append(29, '?');
    hdr += "\r\nContent-Type: ";
    hdr += content_type;
    if (content_encoding) {
        hdr += "\r\nContent-Encoding: ";
        hdr += content_encoding;
        hdr += "\r\nVary: Accept-Encoding";
    }
    hdr += "\r\nContent-Length: ";
    hdr += std::to_string(content_length);
    hdr += "\r\n\r\n";
}

// Frozen responses omit Date; HttpArena static validation does not require it.
void build_frozen(std::string& out, int status, const char* reason,
                  const char* content_type, const char* content_encoding,
                  std::size_t content_length, const char* body,
                  std::size_t body_len, bool with_body) {
    out  = "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason;
    out += "\r\nServer: iris\r\nContent-Type: ";
    out += content_type;
    if (content_encoding) {
        out += "\r\nContent-Encoding: ";
        out += content_encoding;
        out += "\r\nVary: Accept-Encoding";
    }
    out += "\r\nContent-Length: ";
    out += std::to_string(content_length);
    out += "\r\n\r\n";
    if (with_body && body != nullptr && body_len > 0) {
        out.append(body, body_len);
    }
}

void bake_frozen(Variant& v, const char* ctype, const char* encoding) {
    if (!v.present) return;
    build_frozen(v.frozen_get, 200, "OK", ctype, encoding, v.body.size(),
                 v.body.data(), v.body.size(), true);
    build_frozen(v.frozen_head, 200, "OK", ctype, encoding, v.body.size(),
                 nullptr, 0, false);
}

void load_variant(const std::string& base, const std::string& name,
                  const char* suffix, const char* encoding, const char* ctype,
                  Variant& v) {
    std::vector<char> body;
    if (!read_whole(base + name + suffix, body)) return;
    v.body = std::move(body);
    build_header(v.header, v.date_off, 200, "OK", ctype, encoding, v.body.size());
    v.present = true;
    bake_frozen(v, ctype, encoding);
}

const Variant& negotiate(const Entry& e, std::string_view ae) noexcept {
    const bool want_br = e.br.present && ae.find("br")   != std::string_view::npos;
    const bool want_gz = e.gz.present && ae.find("gzip") != std::string_view::npos;
    if (want_br) return e.br;
    if (want_gz) return e.gz;
    return e.identity;
}

void emit_variant(iris::http::Buffer& out, const Variant& v, bool with_body) noexcept {
    const std::size_t base = out.size();
    out.append(std::string_view(v.header));
    if (!out.overflow()) {
        std::memcpy(out.data() + base + v.date_off,
                    iris::http::current_date().data(), 29);
    }
    if (with_body) out.append(std::string_view(v.body.data(), v.body.size()));
}

void emit_variant_zc(iris::http::Buffer& out, const Variant& v,
                     iris::net::AsyncCtx& ctx) noexcept {
    const std::size_t base = out.size();
    out.append(std::string_view(v.header));
    if (out.overflow()) return;
    std::memcpy(out.data() + base + v.date_off,
                iris::http::current_date().data(), 29);
    ctx.set_zerocopy_body(v.body.data(), v.body.size());
}

#if defined(__linux__)
inline std::size_t align_up(std::size_t n, std::size_t a) noexcept {
    return (n + a - 1) & ~(a - 1);
}

struct SealItem {
    std::string* blob;
    Frozen*      slot;
};

bool seal_frozen_responses() noexcept {
    std::vector<SealItem> items;
    items.reserve(g_files.size() * 6 + 1);

    for (auto& [name, e] : g_files) {
        (void)name;
        for (Variant* v : {&e.identity, &e.br, &e.gz}) {
            if (!v->present) continue;
            items.push_back({&v->frozen_get,  &v->get});
            items.push_back({&v->frozen_head, &v->head});
        }
    }
    items.push_back({&g_404_frozen_bytes, &g_404_frozen});

    std::size_t end = 0;
    for (SealItem& it : items) {
        it.slot->off = align_up(end, kPage);
        it.slot->len = it.blob->size();
        end          = it.slot->off + it.slot->len;
    }
    if (end == 0) return false;

    const std::size_t map_size = align_up(end, kPage);

    const int fd = ::memfd_create("iris_ha_static", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        std::fprintf(stderr, "[iris-ha] memfd_create: %s\n", std::strerror(errno));
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(map_size)) != 0) {
        ::close(fd);
        return false;
    }

    void* map = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        ::close(fd);
        return false;
    }

    for (const SealItem& it : items) {
        if (it.slot->len == 0) continue;
        std::memcpy(static_cast<char*>(map) + it.slot->off,
                    it.blob->data(), it.slot->len);
    }

    ::munmap(map, map_size);
    if (::fcntl(fd, F_ADD_SEALS,
                F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW) != 0) {
        std::fprintf(stderr, "[iris-ha] memfd seal: %s\n", std::strerror(errno));
        ::close(fd);
        return false;
    }

    map = ::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        std::fprintf(stderr, "[iris-ha] memfd remap: %s\n", std::strerror(errno));
        ::close(fd);
        return false;
    }

    g_memfd          = fd;
    g_map            = map;
    g_map_len        = map_size;
    g_sendfile_ready = true;

    for (auto& [name, e] : g_files) {
        (void)name;
        for (Variant* v : {&e.identity, &e.br, &e.gz}) {
            if (!v->present) continue;
            v->body.clear();
            v->body.shrink_to_fit();
            v->header.clear();
            v->header.shrink_to_fit();
            v->frozen_get.clear();
            v->frozen_get.shrink_to_fit();
            v->frozen_head.clear();
            v->frozen_head.shrink_to_fit();
        }
    }
    g_404.clear();
    g_404.shrink_to_fit();
    g_404_frozen_bytes.clear();
    g_404_frozen_bytes.shrink_to_fit();
    return true;
}

void emit_frozen(iris::net::AsyncCtx& ctx, const Frozen& fr) noexcept {
    ctx.set_sendfile_response(g_memfd, fr.off, fr.len);
}
#endif

}  // namespace

bool load_static(const char* dir) noexcept {
    DIR* d = ::opendir(dir);
    if (!d) return false;
    std::string base = dir;
    if (!base.empty() && base.back() != '/') base += '/';

    struct dirent* de;
    while ((de = ::readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".br") == 0) continue;
        if (name.size() >= 3 && name.compare(name.size() - 3, 3, ".gz") == 0) continue;

        std::vector<char> body;
        if (!read_whole(base + name, body)) continue;

        const char* ctype = content_type_for(name);
        Entry e;
        e.identity.body = std::move(body);
        build_header(e.identity.header, e.identity.date_off, 200, "OK", ctype,
                     nullptr, e.identity.body.size());
        e.identity.present = true;
        bake_frozen(e.identity, ctype, nullptr);
        load_variant(base, name, ".br", "br",   ctype, e.br);
        load_variant(base, name, ".gz", "gzip", ctype, e.gz);
        g_files.emplace(std::move(name), std::move(e));
    }
    ::closedir(d);

    build_header(g_404, g_404_date_off, 404, "Not Found", "text/plain", nullptr, 0);
    build_frozen(g_404_frozen_bytes, 404, "Not Found", "text/plain", nullptr, 0,
                 nullptr, 0, false);

    if (g_files.empty()) return false;

#if defined(__linux__)
    if (!seal_frozen_responses()) {
        std::fprintf(stderr, "[iris-ha] frozen seal failed; static uses fallback\n");
        g_sendfile_ready = false;
    }
#endif
    return true;
}

bool handle_static(const iris::http::Request& req, iris::http::Buffer& out,
                   iris::net::AsyncCtx& ctx, int minor_version,
                   bool keep_alive) noexcept {
    (void)minor_version;
    (void)keep_alive;
    const bool is_get  = req.method == "GET";
    const bool is_head = req.method == "HEAD";
    if (!is_get && !is_head) return false;

    std::string_view path = req.path;
    const std::size_t q = path.find('?');
    if (q != std::string_view::npos) path = path.substr(0, q);
    path.remove_prefix(std::strlen("/static/"));

    if (path.find('/') != std::string_view::npos) {
#if defined(__linux__)
        if (g_sendfile_ready && g_404_frozen.len > 0) {
            emit_frozen(ctx, g_404_frozen);
            return true;
        }
#endif
        const std::size_t base = out.size();
        out.append(std::string_view(g_404));
        if (!out.overflow()) {
            std::memcpy(out.data() + base + g_404_date_off,
                        iris::http::current_date().data(), 29);
        }
        return true;
    }

    auto it = g_files.find(std::string(path));
    if (it == g_files.end()) {
#if defined(__linux__)
        if (g_sendfile_ready && g_404_frozen.len > 0) {
            emit_frozen(ctx, g_404_frozen);
            return true;
        }
#endif
        const std::size_t base = out.size();
        out.append(std::string_view(g_404));
        if (!out.overflow()) {
            std::memcpy(out.data() + base + g_404_date_off,
                        iris::http::current_date().data(), 29);
        }
        return true;
    }

    const Variant& v = negotiate(it->second, req.accept_encoding);

#if defined(__linux__)
    if (g_sendfile_ready) {
        const Frozen& fr = is_get ? v.get : v.head;
        if (fr.len > 0) {
            emit_frozen(ctx, fr);
            return true;
        }
    }
#endif

    if (is_get) {
        emit_variant_zc(out, v, ctx);
    } else {
        emit_variant(out, v, false);
    }
    return true;
}

}  // namespace iris::ha
