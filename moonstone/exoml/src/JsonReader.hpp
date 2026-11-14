#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moonstone::exoml::contract::detail {

enum class JsonType { Null, Bool, Int, Double, String, Array, Object };

struct JsonValue {
    JsonType type = JsonType::Null;
    bool bool_value = false;
    std::int64_t int_value = 0;
    double double_value = 0.0;
    std::string string_value;
    std::vector<JsonValue> array_value;
    std::vector<std::pair<std::string, JsonValue>> object_value;

    const JsonValue &operator[](std::string_view key) const;
    const JsonValue &operator[](std::size_t index) const;
    bool contains(std::string_view key) const;
    std::size_t size() const;

    std::string as_string() const;
    std::int64_t as_int() const;
    bool as_bool() const;
    double as_number() const;
};

// Budget-limited recursive-descent JSON parser. Throws std::invalid_argument
// on syntax errors, type mismatches, or budget exhaustion.
JsonValue parse_json(std::string_view input);

} // namespace moonstone::exoml::contract::detail
