#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Version.hpp"

// Result/Error are the Moonstone core error model. Mahak deliberately reuses
// them instead of introducing a parallel error hierarchy.
#if __has_include("../../../core/Result.hpp")
#include "../../../core/Result.hpp"
#else
#include <moonstone/core/Result.hpp>
#endif

namespace moonstone::mahak {
inline namespace v1 {

// A strict, dependency-free JSON value used for manifests and result files.
// Object keys are stored in a std::map so serialization is deterministic.
// Numbers preserve the distinction between exact signed integers and IEEE
// doubles. Non-finite numbers are rejected on both parse and serialization.
class Json {
  public:
    enum class Type { Null, Boolean, Integer, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json, std::less<>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Boolean), boolean_(value) {}
    Json(int value) : type_(Type::Integer), integer_(value) {}
    Json(long value) : type_(Type::Integer), integer_(value) {}
    Json(long long value) : type_(Type::Integer), integer_(value) {}
    Json(unsigned value) : type_(Type::Integer), integer_(static_cast<std::int64_t>(value)) {}
    Json(unsigned long value) : type_(Type::Integer), integer_(static_cast<std::int64_t>(value)) {}
    Json(unsigned long long value)
        : type_(Type::Integer), integer_(static_cast<std::int64_t>(value)) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(const char *value) : type_(Type::String), string_(value ? value : "") {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(std::string_view value) : type_(Type::String), string_(value) {}
    Json(Array value) : type_(Type::Array), array_(std::move(value)) {}
    Json(Object value) : type_(Type::Object), object_(std::move(value)) {}

    static Json array() {
        Json value;
        value.type_ = Type::Array;
        return value;
    }
    static Json object() {
        Json value;
        value.type_ = Type::Object;
        return value;
    }

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Boolean; }
    bool is_integer() const noexcept { return type_ == Type::Integer; }
    bool is_number() const noexcept { return type_ == Type::Number || type_ == Type::Integer; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    std::optional<bool> as_bool() const;
    std::optional<std::int64_t> as_int64() const;
    std::optional<std::uint64_t> as_uint64() const;
    std::optional<double> as_double() const;
    const std::string *as_string() const noexcept;
    const Array *as_array() const noexcept;
    const Object *as_object() const noexcept;
    Array *as_array() noexcept;
    Object *as_object() noexcept;

    // Object mutation. set() converts a non-object to an empty object only when
    // explicitly requested; otherwise callers should construct object() first.
    Json &set(std::string key, Json value);
    Json &push(Json value);

    // Missing keys return nullptr. This is a lookup, not a schema validator.
    const Json *find(std::string_view key) const noexcept;
    Json *find(std::string_view key) noexcept;

    // A deterministic serialization. pretty=false emits no insignificant
    // whitespace. dump() returns an empty string and sets *ok=false on an
    // internal error (never for a valid value).
    std::string dump(bool pretty = false, unsigned indent = 2, bool *ok = nullptr) const;

    static Result<Json> parse(std::string_view text, std::string_view source = "<json>");

    friend bool operator==(const Json &a, const Json &b) noexcept;

  private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    std::int64_t integer_ = 0;
    double number_ = 0.0;
    std::string string_;
    Array array_;
    Object object_;

    friend class JsonWriter;
};

// Convenience accessors that make malformed result files explicit.
Result<bool> require_bool(const Json &value, std::string_view path);
Result<std::int64_t> require_int64(const Json &value, std::string_view path);
Result<std::uint64_t> require_uint64(const Json &value, std::string_view path);
Result<double> require_double(const Json &value, std::string_view path);
Result<std::string> require_string(const Json &value, std::string_view path);
Result<const Json::Array *> require_array(const Json &value, std::string_view path);
Result<const Json::Object *> require_object(const Json &value, std::string_view path);

// Strict UTF-8 validation used by the JSON reader/writer and by result import.
bool valid_utf8(std::string_view text) noexcept;

} // namespace v1
} // namespace moonstone::mahak
