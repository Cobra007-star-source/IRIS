// =============================================================================
// bench/conformance.cpp
//
// JSON Schema Test Suite 合规性 runner
//
// 走一遍 https://github.com/json-schema-org/JSON-Schema-Test-Suite
// 的 draft2020-12 目录，把每个 case 的 schema 喂给 IRIS，再把每条 instance
// 跑过 Validator，与官方 expected 对比。
//
// 输出按文件分类的表格 + 三种口径的合格率：
//
//   raw     = pass / total                  // 严格视角（skipped 算失败）
//   attempt = pass / (pass + fail)          // 我们答了的题答对率
//   skip%   = skipped / total               // schema 编译失败比（不支持的关键字）
//
// 用法：
//   conformance <draft_dir> [--verbose]
//   conformance .test-suite/tests/draft2020-12 --verbose
// =============================================================================
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "iris/json_reader.hpp"
#include "iris/schema.hpp"
#include "iris/slow_schema.hpp"
#include "iris/validator.hpp"

namespace fs = std::filesystem;

namespace {

void serialize(const iris::JsonValue& v, std::string& out);

void serialize_string(std::string_view s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            default:
                if (c < 0x20) {
                    char buf[8]; std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void serialize(const iris::JsonValue& v, std::string& out) {
    using T = iris::JsonValue::Type;
    switch (v.type()) {
        case T::kNull:   out += "null"; return;
        case T::kBool:   out += v.as_bool() ? "true" : "false"; return;
        case T::kInt: {
            char buf[32]; std::snprintf(buf, sizeof(buf), "%lld",
                                        static_cast<long long>(v.as_int()));
            out += buf; return;
        }
        case T::kDouble: {
            char buf[32]; std::snprintf(buf, sizeof(buf), "%.17g", v.as_double());
            out += buf; return;
        }
        case T::kString: serialize_string(v.as_string(), out); return;
        case T::kArray: {
            out.push_back('[');
            bool first = true;
            for (auto& e : v.as_array()) {
                if (!first) out.push_back(',');
                first = false;
                serialize(e, out);
            }
            out.push_back(']'); return;
        }
        case T::kObject: {
            out.push_back('{');
            bool first = true;
            for (auto& [k, val] : v.as_object()) {
                if (!first) out.push_back(',');
                first = false;
                serialize_string(k, out);
                out.push_back(':');
                serialize(val, out);
            }
            out.push_back('}'); return;
        }
    }
}

struct FileStats {
    std::string name;
    int total   = 0;
    int passed  = 0;
    int failed  = 0;
    int skipped = 0;   // schema 不能编译（无 fast 也无 slow）
    int errored = 0;
    int by_fast = 0;   // 被 Fast Path 接管的题
    int by_slow = 0;   // 被 Slow Path 接管的题
};

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

// 收集 .test-suite/remotes/** 下所有 .json 文件，按 "http://localhost:1234/<rel>"
// 的 URI 注入到 Validator。返回 (uri, content) 列表，多个 Validator 共享之。
using RemoteList = std::vector<std::pair<std::string, std::string>>;

RemoteList load_remotes(const fs::path& remotes_dir) {
    RemoteList out;
    if (!fs::is_directory(remotes_dir)) return out;
    for (auto it = fs::recursive_directory_iterator(remotes_dir);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        if (it->path().extension() != ".json") continue;
        std::string rel = fs::relative(it->path(), remotes_dir).generic_string();
        std::string uri = "http://localhost:1234/" + rel;
        out.emplace_back(std::move(uri), slurp(it->path()));
    }
    return out;
}

void run_file(const fs::path& p, FileStats& s, bool verbose, const RemoteList& remotes) {
    s.name = p.filename().string();
    auto parsed = iris::parse_json(slurp(p));
    if (!parsed.ok || !parsed.value.is_array()) { ++s.errored; return; }

    for (auto& case_v : parsed.value.as_array()) {
        if (!case_v.is_object()) continue;
        const iris::JsonValue* schema_v = case_v.find("schema");
        const iris::JsonValue* tests_v  = case_v.find("tests");
        const iris::JsonValue* desc_v   = case_v.find("description");
        if (!schema_v || !tests_v || !tests_v->is_array()) continue;

        std::string schema_str;
        serialize(*schema_v, schema_str);

        iris::ValidatorBuild vb;
        iris::Validator vv = iris::Validator::from_schema_json(schema_str, vb);
        bool used_slow = vv.has_slow_path();

        // 给 Slow Path 灌远端文档：$ref 解析需要
        if (used_slow) {
            for (auto& [uri, body] : remotes) {
                vv.add_remote_document(uri, body);
            }
        }

        for (auto& t : tests_v->as_array()) {
            ++s.total;
            const auto* instance = t.find("data");
            const auto* valid_v  = t.find("valid");
            const auto* tdesc    = t.find("description");
            if (!instance || !valid_v || !valid_v->is_bool()) { ++s.errored; continue; }
            bool expected = valid_v->as_bool();

            if (!vb.ok) { ++s.skipped; continue; }

            std::string instance_str;
            serialize(*instance, instance_str);

            auto r = vv.validate(instance_str);
            bool got = r.ok();

            if (used_slow) ++s.by_slow; else ++s.by_fast;

            if (got == expected) {
                ++s.passed;
            } else {
                ++s.failed;
                if (verbose) {
                    std::fprintf(stderr, "  FAIL [%s] %s :: %s :: %s :: expect=%s got=%s (code=%s)\n",
                                 used_slow ? "slow" : "fast",
                                 s.name.c_str(),
                                 desc_v && desc_v->is_string() ? desc_v->as_string().c_str() : "?",
                                 tdesc && tdesc->is_string()   ? tdesc->as_string().c_str()  : "?",
                                 expected ? "valid" : "invalid",
                                 got      ? "valid" : "invalid",
                                 iris::validation_error_name(r.code));
                }
            }
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <draft_dir> [--verbose]\n", argv[0]);
        return 2;
    }
    bool verbose = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--verbose") == 0) verbose = true;
    }
    fs::path dir = argv[1];
    if (!fs::is_directory(dir)) {
        std::fprintf(stderr, "not a directory: %s\n", dir.c_str());
        return 2;
    }

    // 找 .test-suite/remotes/，路径相对于 dir 向上两层。允许 --remotes 覆盖。
    fs::path remotes_dir;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--remotes") == 0 && i + 1 < argc) {
            remotes_dir = argv[i + 1];
            ++i;
        }
    }
    if (remotes_dir.empty()) {
        fs::path candidate = dir.parent_path().parent_path() / "remotes";
        if (fs::is_directory(candidate)) remotes_dir = candidate;
    }
    RemoteList remotes = load_remotes(remotes_dir);
    if (!remotes.empty()) {
        std::fprintf(stderr, "loaded %zu remote schemas from %s\n",
                     remotes.size(), remotes_dir.c_str());
    }

    std::vector<FileStats> per_file;
    for (auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".json") {
            FileStats s{};
            run_file(e.path(), s, verbose, remotes);
            per_file.push_back(std::move(s));
        }
    }
    std::sort(per_file.begin(), per_file.end(),
              [](const FileStats& a, const FileStats& b) { return a.name < b.name; });

    int t_total=0, t_pass=0, t_fail=0, t_skip=0, t_err=0, t_fast=0, t_slow=0;
    std::printf("%-32s | %5s | %5s | %5s | %7s | %5s | %5s | %5s\n",
                "file", "total", "pass", "fail", "skipped", "err", "fast", "slow");
    std::printf("---------------------------------+-------+-------+-------+---------+-------+-------+------\n");
    for (auto& s : per_file) {
        std::printf("%-32s | %5d | %5d | %5d | %7d | %5d | %5d | %5d\n",
                    s.name.c_str(), s.total, s.passed, s.failed, s.skipped, s.errored,
                    s.by_fast, s.by_slow);
        t_total += s.total; t_pass += s.passed; t_fail += s.failed;
        t_skip  += s.skipped; t_err  += s.errored;
        t_fast  += s.by_fast; t_slow += s.by_slow;
    }
    std::printf("---------------------------------+-------+-------+-------+---------+-------+-------+------\n");
    std::printf("%-32s | %5d | %5d | %5d | %7d | %5d | %5d | %5d\n",
                "TOTAL", t_total, t_pass, t_fail, t_skip, t_err, t_fast, t_slow);

    std::printf("\n--- Pass rates ---\n");
    if (t_total > 0) {
        std::printf("raw       : %d / %d = %.2f%%   (skipped aligned to failure)\n",
                    t_pass, t_total, 100.0 * t_pass / t_total);
    }
    int attempted = t_pass + t_fail;
    if (attempted > 0) {
        std::printf("attempted : %d / %d = %.2f%%   (when IRIS answered, how often right)\n",
                    t_pass, attempted, 100.0 * t_pass / attempted);
    }
    if (t_total > 0) {
        std::printf("skipped%%  : %d / %d = %.2f%%   (schema rejected by both fast & slow path)\n",
                    t_skip, t_total, 100.0 * t_skip / t_total);
        std::printf("fast/slow : %d fast, %d slow (%.2f%% / %.2f%% of attempted)\n",
                    t_fast, t_slow,
                    100.0 * t_fast / std::max(1, attempted),
                    100.0 * t_slow / std::max(1, attempted));
    }
    return 0;
}
