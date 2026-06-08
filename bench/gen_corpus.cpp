// =============================================================================
// bench/gen_corpus.cpp
//
// 生成 ajv-style benchmark 语料。两组：
//
//   flat/    扁平 4 字段 person schema（与 ajv 对比的"小而典型"）
//   nested/  含嵌套 object + array-of-object 的"接近真实业务"语料
//
// 用法：
//   gen_corpus <out_dir> [count=1000000]
//
// 输出文件：
//   <out_dir>/flat/{schema.json, data.jsonl, bad.jsonl}
//   <out_dir>/nested/{schema.json, data.jsonl}
//
// 所有随机使用固定 seed=0xC0FFEE，保证跨平台可复现。
// =============================================================================
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr const char* kFlatSchemaJson = R"({
  "type": "object",
  "properties": {
    "name":   {"type": "string",  "minLength": 1,  "maxLength": 64},
    "age":    {"type": "integer", "minimum": 0,    "maximum": 200},
    "email":  {"type": "string",  "minLength": 3,  "maxLength": 128},
    "active": {"type": "boolean"}
  },
  "required": ["name", "age"],
  "additionalProperties": false
}
)";

constexpr const char* kNestedSchemaJson = R"({
  "type": "object",
  "properties": {
    "id":     {"type": "integer", "minimum": 0},
    "name":   {"type": "string",  "minLength": 1, "maxLength": 64},
    "addr":   {
      "type": "object",
      "properties": {
        "city":    {"type": "string", "minLength": 1, "maxLength": 32},
        "country": {"type": "string", "minLength": 2, "maxLength": 32},
        "zip":     {"type": "string", "maxLength": 16}
      },
      "required": ["city"],
      "additionalProperties": false
    },
    "tags":   {"type": "array", "items": {"type": "string"}},
    "events": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "ts":   {"type": "integer", "minimum": 0},
          "kind": {"type": "string"}
        },
        "required": ["ts", "kind"]
      }
    }
  },
  "required": ["id", "name"],
  "additionalProperties": false
}
)";

const std::string kFirstNames[] = {
    "Iris","Alice","Bob","Charlie","Diana","Eve","Frank","Grace","Henry",
    "Ivy","Jack","Kate","Leo","Mona","Nate","Olive","Pete","Quinn","Ruby",
    "Sam","Tina","Uma","Vic","Wendy","Xena","Yan","Zoe",
};
const std::string kLastNames[]  = {
    "Lin","Wang","Zhang","Smith","Johnson","Brown","Garcia","Davis",
    "Miller","Wilson","Moore","Anderson",
};
const std::string kDomains[]    = {
    "iris.dev","example.com","test.org","mail.io","fastmail.cc",
};

std::string make_name(std::mt19937_64& rng) {
    const auto& a = kFirstNames[rng() % (sizeof(kFirstNames)/sizeof(kFirstNames[0]))];
    const auto& b = kLastNames[rng()  % (sizeof(kLastNames)/sizeof(kLastNames[0]))];
    return a + " " + b;
}

std::string make_email(std::mt19937_64& rng) {
    const auto& a = kFirstNames[rng() % (sizeof(kFirstNames)/sizeof(kFirstNames[0]))];
    const auto& d = kDomains[rng()    % (sizeof(kDomains)/sizeof(kDomains[0]))];
    std::string lower; lower.reserve(a.size());
    for (char c : a) lower.push_back(static_cast<char>(c | 0x20));
    char buf[8]; std::snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(rng() % 1000));
    return lower + buf + "@" + d;
}

void emit_valid_record(std::ostream& out, std::mt19937_64& rng) {
    out << R"({"name":")" << make_name(rng)
        << R"(","age":)"  << (rng() % 201);
    bool has_email = (rng() & 3u) != 0;
    bool has_act   = (rng() & 1u) != 0;
    if (has_email) out << R"(,"email":")" << make_email(rng) << R"(")";
    if (has_act)   out << R"(,"active":)" << ((rng() & 1u) ? "true" : "false");
    out << "}\n";
}

const std::string kCities[]   = {"SF","NY","LA","SEA","CHI","Beijing","Shanghai","Tokyo","London","Paris"};
const std::string kCountries[]= {"US","CN","JP","UK","FR","DE","CA","AU"};
const std::string kEventKinds[]={"login","click","purchase","logout","view","share"};

template <typename T, std::size_t N>
const T& pick(const T (&arr)[N], std::mt19937_64& rng) {
    return arr[rng() % N];
}

void emit_nested_record(std::ostream& out, std::mt19937_64& rng) {
    out << R"({"id":)" << (rng() % 1'000'000)
        << R"(,"name":")" << make_name(rng) << R"(")";
    if ((rng() & 1u) != 0) {  // addr 可选
        out << R"(,"addr":{"city":")" << pick(kCities, rng) << R"(")";
        if ((rng() & 1u) != 0) out << R"(,"country":")" << pick(kCountries, rng) << R"(")";
        if ((rng() & 3u) == 0) out << R"(,"zip":")" << (rng() % 100000) << R"(")";
        out << "}";
    }
    if ((rng() & 1u) != 0) {  // tags 可选
        out << R"(,"tags":[)";
        int n = static_cast<int>(rng() % 4);
        for (int i = 0; i < n; ++i) {
            if (i) out << ",";
            out << R"(")" << pick(kEventKinds, rng) << R"(")";
        }
        out << "]";
    }
    if ((rng() & 3u) != 0) {  // events 经常出现
        out << R"(,"events":[)";
        int n = static_cast<int>(rng() % 4);
        for (int i = 0; i < n; ++i) {
            if (i) out << ",";
            out << R"({"ts":)" << (rng() % 1'700'000'000)
                << R"(,"kind":")" << pick(kEventKinds, rng) << R"("})";
        }
        out << "]";
    }
    out << "}\n";
}

void emit_bad_record(std::ostream& out, std::mt19937_64& rng) {
    // 各种违反方式按经验比例混合
    int kind = static_cast<int>(rng() % 6);
    switch (kind) {
        case 0:  // missing required age
            out << R"({"name":")" << make_name(rng) << R"("})" << "\n"; break;
        case 1:  // type mismatch age
            out << R"({"name":")" << make_name(rng) << R"(","age":"oops"})" << "\n"; break;
        case 2:  // unknown field
            out << R"({"name":")" << make_name(rng)
                << R"(","age":1,"extra":42})" << "\n"; break;
        case 3:  // age out of range
            out << R"({"name":")" << make_name(rng)
                << R"(","age":)" << (1000 + (rng() % 1000)) << "}\n"; break;
        case 4:  // bad json
            out << R"({"name":")" << make_name(rng)
                << R"(","age":,})" << "\n"; break;
        default: // duplicate field
            out << R"({"name":"a","age":1,"name":"b"})" << "\n"; break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <out_dir> [count=1000000]\n", argv[0]);
        return 2;
    }
    fs::path out_dir = argv[1];
    std::size_t count = (argc > 2) ? std::strtoull(argv[2], nullptr, 10) : 1'000'000;
    fs::create_directories(out_dir / "flat");
    fs::create_directories(out_dir / "nested");

    // ---- flat ----
    {
        std::ofstream s(out_dir / "flat" / "schema.json", std::ios::binary | std::ios::trunc);
        s << kFlatSchemaJson;
    }
    std::mt19937_64 rng_a(0xC0FFEE);
    {
        std::ofstream f(out_dir / "flat" / "data.jsonl", std::ios::binary | std::ios::trunc);
        for (std::size_t i = 0; i < count; ++i) emit_valid_record(f, rng_a);
    }
    {
        std::ofstream f(out_dir / "flat" / "bad.jsonl", std::ios::binary | std::ios::trunc);
        std::size_t n_bad = std::max<std::size_t>(count / 10, 1);
        for (std::size_t i = 0; i < n_bad; ++i) emit_bad_record(f, rng_a);
    }
    std::printf("[gen] flat   schema + data(%zu) + bad(%zu)\n",
                count, std::max<std::size_t>(count/10, 1));

    // ---- nested ----
    {
        std::ofstream s(out_dir / "nested" / "schema.json", std::ios::binary | std::ios::trunc);
        s << kNestedSchemaJson;
    }
    std::mt19937_64 rng_b(0xC0FFEE ^ 0xBADu);
    {
        std::ofstream f(out_dir / "nested" / "data.jsonl", std::ios::binary | std::ios::trunc);
        for (std::size_t i = 0; i < count; ++i) emit_nested_record(f, rng_b);
    }
    std::printf("[gen] nested schema + data(%zu)\n", count);

    return 0;
}
