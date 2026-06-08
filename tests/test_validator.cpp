// =============================================================================
// tests/test_validator.cpp
// =============================================================================
#include <array>

#include "iris/validator.hpp"
#include "test_framework.hpp"

namespace {

iris::Validator make_basic() {
    using namespace iris;
    std::array<FieldSpec, 4> fields = {{
        {"name",   kTypeString,  .required = true,  .min_string_len = 1, .max_string_len = 16},
        {"age",    kTypeInteger, .required = true,  .min_int = 0,        .max_int = 200},
        {"email",  kTypeString,  .required = false, .min_string_len = 3, .max_string_len = 64},
        {"active", kTypeBoolean, .required = false},
    }};
    auto built = compile_schema(fields, false);
    if (!built.ok) {
        std::fprintf(stderr, "make_basic: compile_schema failed: %s\n",
                     built.diagnostic.c_str());
        std::abort();
    }
    return Validator(std::move(built.schema));
}

}  // namespace

IRIS_TEST(validate_minimum_ok) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"Iris","age":3})");
    IRIS_EXPECT(r.ok());
}

IRIS_TEST(validate_full_ok) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"Iris","age":24,"email":"a@b.cd","active":true})");
    IRIS_EXPECT(r.ok());
}

IRIS_TEST(validate_missing_required) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"Iris"})");
    IRIS_EXPECT_EQ(r.code, iris::ValidationError::kMissingRequired);
}

IRIS_TEST(validate_unknown_field) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"Iris","age":1,"xx":1})");
    IRIS_EXPECT_EQ(r.code, iris::ValidationError::kUnknownField);
}

IRIS_TEST(validate_type_mismatch) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"Iris","age":"oops"})");
    IRIS_EXPECT_EQ(r.code, iris::ValidationError::kTypeMismatch);
}

IRIS_TEST(validate_string_too_long) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"thisisaverylongnamethatexceeds16","age":1})");
    IRIS_EXPECT_EQ(r.code, iris::ValidationError::kStringTooLong);
}

IRIS_TEST(validate_int_out_of_range) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"Iris","age":-1})");
    IRIS_EXPECT_EQ(r.code, iris::ValidationError::kIntOutOfRange);
}

IRIS_TEST(validate_duplicate_field) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"a","age":1,"name":"b"})");
    IRIS_EXPECT_EQ(r.code, iris::ValidationError::kDuplicateField);
}

IRIS_TEST(validate_invalid_json) {
    auto v = make_basic();
    auto r = v.validate(R"({"name":"a",,"age":1})");
    IRIS_EXPECT(!r.ok());
}

IRIS_TEST(validate_whitespace_and_unicode) {
    auto v = make_basic();
    auto r = v.validate("  {\n  \"name\" : \"X\" ,\n  \"age\" : 7\n}  ");
    IRIS_EXPECT(r.ok());
}

IRIS_TEST(validate_nested_value_skipped) {
    // schema 不允许 active 之外的字段，但允许的字段值如果是对象时，会被 skip_balanced 跳过
    using namespace iris;
    std::array<FieldSpec, 2> fields = {{
        {"name", kTypeString, .required = true, .max_string_len = 16},
        {"meta", kTypeObject, .required = false},
    }};
    auto b = compile_schema(fields, false);
    Validator vv(std::move(b.schema));
    auto r = vv.validate(R"({"name":"a","meta":{"k":[1,2,{"x":"y"}],"q":"with \"quote\" inside"}})");
    IRIS_EXPECT(r.ok());
}

IRIS_TEST(validate_nested_object_schema) {
    using namespace iris;
    constexpr const char* schema_json = R"({
      "type": "object",
      "properties": {
        "name": {"type": "string", "minLength": 1, "maxLength": 32},
        "addr": {
          "type": "object",
          "properties": {
            "city": {"type": "string", "minLength": 1},
            "zip":  {"type": "string", "maxLength": 10}
          },
          "required": ["city"],
          "additionalProperties": false
        }
      },
      "required": ["name"],
      "additionalProperties": false
    })";
    auto built = compile_schema_from_json(schema_json);
    IRIS_EXPECT(built.ok);
    if (!built.ok) { std::fprintf(stderr, "schema err: %s\n", built.diagnostic.c_str()); return; }
    Validator v(std::move(built.schema));

    IRIS_EXPECT(v.validate(R"({"name":"Iris","addr":{"city":"SF","zip":"94101"}})").ok());
    IRIS_EXPECT(v.validate(R"({"name":"Iris","addr":{"city":"SF"}})").ok());
    // nested 缺少 required.city
    auto r1 = v.validate(R"({"name":"Iris","addr":{"zip":"94101"}})");
    IRIS_EXPECT(r1.code == ValidationError::kMissingRequired);
    // nested 多余字段（addr.additionalProperties=false）
    auto r2 = v.validate(R"({"name":"Iris","addr":{"city":"SF","country":"US"}})");
    IRIS_EXPECT(r2.code == ValidationError::kUnknownField);
    // nested 类型错（zip 不是字符串）
    auto r3 = v.validate(R"({"name":"Iris","addr":{"city":"SF","zip":12345}})");
    IRIS_EXPECT(r3.code == ValidationError::kTypeMismatch);
}

IRIS_TEST(validate_array_of_strings) {
    using namespace iris;
    constexpr const char* schema_json = R"({
      "type": "object",
      "properties": {
        "tags": {"type": "array", "items": {"type": "string"}}
      },
      "required": ["tags"]
    })";
    auto built = compile_schema_from_json(schema_json);
    IRIS_EXPECT(built.ok);
    Validator v(std::move(built.schema));

    IRIS_EXPECT(v.validate(R"({"tags":["a","b","c"]})").ok());
    IRIS_EXPECT(v.validate(R"({"tags":[]})").ok());
    // 一个 item 类型错
    auto r = v.validate(R"({"tags":["a",2,"c"]})");
    IRIS_EXPECT(r.code == ValidationError::kTypeMismatch);
}

IRIS_TEST(validate_array_of_objects) {
    using namespace iris;
    constexpr const char* schema_json = R"({
      "type": "object",
      "properties": {
        "items": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "id":   {"type": "integer", "minimum": 0},
              "name": {"type": "string"}
            },
            "required": ["id"]
          }
        }
      }
    })";
    auto built = compile_schema_from_json(schema_json);
    IRIS_EXPECT(built.ok);
    Validator v(std::move(built.schema));

    IRIS_EXPECT(v.validate(R"({"items":[{"id":1,"name":"a"},{"id":2}]})").ok());
    auto r = v.validate(R"({"items":[{"name":"orphan"}]})");
    IRIS_EXPECT(r.code == ValidationError::kMissingRequired);
}
