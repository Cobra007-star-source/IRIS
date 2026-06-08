// =============================================================================
// bench/bowtie_iris.cpp
//
// Bowtie (https://docs.bowtie.report) JSON-RPC stdio harness driver.
//
// Bowtie 协议：每行 stdin 一个 JSON 命令对象，每行 stdout 一个 JSON 响应对象。
//
//   {"cmd":"start", "version":1}
//   → {"version":1, "implementation":{...}}
//
//   {"cmd":"dialect", "dialect":"https://json-schema.org/draft/2020-12/schema"}
//   → {"ok":true}
//
//   {"cmd":"run", "seq":N, "case":{"description":"...", "schema":{...}, "tests":[{"instance":..., "valid":bool}, ...]}}
//   → {"seq":N, "results":[{"valid":bool}, ...]}
//
//   {"cmd":"stop"}
//   → exit
//
// IRIS 的 Bowtie 集成是 Phase 4 的核心交付，目的是出现在 Bowtie 报告页：
//   https://bowtie.report
//
// 当前限制（详见响应 implementation block）：
//   - dialect 仅声明支持 draft-2020-12 的语义子集
//   - 不支持的关键字会让 schema 编译失败，driver 回应 "skipped"
// =============================================================================
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

#include "iris/json_reader.hpp"
#include "iris/validator.hpp"

namespace {

// 将 JsonValue 序列化回 JSON 文本（仅 schema/instance 用，无 unicode escape 优化）
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
        case T::kNull: out += "null"; return;
        case T::kBool: out += v.as_bool() ? "true" : "false"; return;
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
            out.push_back(']');
            return;
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
            out.push_back('}');
            return;
        }
    }
}

void write_line(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void on_start() {
    write_line(R"({"version":1,"implementation":{)"
               R"("name":"iris",)"
               R"("language":"cpp",)"
               R"("homepage":"https://github.com/iris-validator/iris",)"
               R"("issues":"https://github.com/iris-validator/iris/issues",)"
               R"("source":"https://github.com/iris-validator/iris",)"
               R"("dialects":["https://json-schema.org/draft/2020-12/schema"])"
               "}}");
}

void on_dialect(const iris::JsonValue& cmd) {
    (void)cmd;
    write_line(R"({"ok":true})");
}

// 主请求处理：run 命令包含一个 case，多条 tests。
// 每条 test 给出 instance 与期望 valid，driver 返回 IRIS 的实际判定。
void on_run(const iris::JsonValue& cmd) {
    std::string out = "{";
    const auto* seq = cmd.find("seq");
    if (seq) { out += "\"seq\":"; serialize(*seq, out); out += ","; }

    const auto* case_v = cmd.find("case");
    if (!case_v || !case_v->is_object()) {
        out += "\"results\":[],\"error\":\"case missing\"}";
        write_line(out);
        return;
    }
    const auto* schema_v = case_v->find("schema");
    const auto* tests_v  = case_v->find("tests");
    if (!schema_v || !tests_v || !tests_v->is_array()) {
        out += "\"results\":[],\"error\":\"schema/tests missing\"}";
        write_line(out);
        return;
    }

    // 编译 schema
    std::string schema_str; serialize(*schema_v, schema_str);
    auto built = iris::compile_schema_from_json(schema_str);

    out += "\"results\":[";
    bool first_r = true;
    for (auto& t : tests_v->as_array()) {
        if (!first_r) out += ",";
        first_r = false;
        if (!built.ok) {
            // schema 编译失败 → Bowtie 期望 implementation 报 skipped
            out += R"({"skipped":true,"message":")";
            // 截断长 diagnostic
            std::string msg = built.diagnostic;
            if (msg.size() > 120) msg.resize(120);
            for (char c : msg) {
                if (c == '"' || c == '\\') out.push_back('\\');
                out.push_back(c);
            }
            out += "\"}";
            continue;
        }
        const auto* inst = t.find("instance");
        if (!inst) { out += R"({"errored":true,"context":{"message":"instance missing"}})"; continue; }
        std::string inst_str; serialize(*inst, inst_str);
        iris::Validator v(iris::CompiledSchema{std::move(built.schema)});
        auto r = v.validate(inst_str);
        out += r.ok() ? R"({"valid":true})" : R"({"valid":false})";
        // Validator 会消费 schema；重新编译以便下一个 test 用同样的 schema
        built = iris::compile_schema_from_json(schema_str);
    }
    out += "]}";
    write_line(out);
}

}  // namespace

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    line.reserve(1 << 16);

    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        auto p = iris::parse_json(line);
        if (!p.ok || !p.value.is_object()) {
            write_line(R"({"error":"bad command line"})");
            continue;
        }
        const auto* cmd_v = p.value.find("cmd");
        if (!cmd_v || !cmd_v->is_string()) {
            write_line(R"({"error":"cmd missing"})");
            continue;
        }
        const std::string& cmd = cmd_v->as_string();
        if (cmd == "start") {
            on_start();
        } else if (cmd == "dialect") {
            on_dialect(p.value);
        } else if (cmd == "run") {
            on_run(p.value);
        } else if (cmd == "stop") {
            write_line("{}");
            return 0;
        } else {
            write_line(R"({"error":"unknown cmd"})");
        }
    }
    return 0;
}
