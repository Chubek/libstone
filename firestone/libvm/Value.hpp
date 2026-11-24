#pragma once
#include "Address.hpp"
#include "../libbignum/BigInt.hpp"
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace firestone::vm {

// Categories describe signatures; representations describe the active payload.
// Both integer representations satisfy an Integer signature.
enum class ValueKind { Null, Boolean, Integer, Float, String, Address, Object };
enum class ValueRepresentation { Null, Boolean, Int64, Float, String, Address, Object, BigInt };
using bignum::BigInt;

namespace value_detail {
using Storage = std::variant<std::monostate, bool, std::int64_t, double, std::string, Address, ObjectRef, BigInt>;

inline bool fits_int64(const BigInt& value) {
    const auto bits = value.bit_length();
    if (bits < 64) return true;
    if (bits != 64 || !value.negative()) return false;
    for (unsigned bit = 0; bit < 63; ++bit) if (value.test_bit(bit)) return false;
    return true; // -2^63
}
} // namespace value_detail

// Keep the existing variant accessors and compact integer payload. Ordinary
// construction chooses a representation by magnitude; explicit in-place
// construction may still be used to select a representation.
class Value : public value_detail::Storage {
    using Storage = value_detail::Storage;

    template<class T> static BigInt wide_integral(T value) {
        using Unsigned = std::make_unsigned_t<T>;
        const auto magnitude = value < 0 ? Unsigned(0) - static_cast<Unsigned>(value) : static_cast<Unsigned>(value);
        BigInt result;
        for (auto bits = std::numeric_limits<Unsigned>::digits; bits > 0;) {
            const auto chunk = bits % 32 ? bits % 32 : 32;
            bits -= chunk;
            const auto limb = static_cast<std::uint32_t>(magnitude >> bits);
            result = (result << chunk) + BigInt(static_cast<std::int64_t>(limb));
        }
        return value < 0 ? -result : std::move(result);
    }
    template<class T> static Storage integral_storage(T value) {
        if constexpr (std::numeric_limits<T>::digits > std::numeric_limits<std::int64_t>::digits) {
            if constexpr (std::is_signed_v<T>) {
                if (value < static_cast<T>(INT64_MIN)) return wide_integral(value);
            }
            if (value > static_cast<T>(INT64_MAX)) return wide_integral(value);
        }
        return static_cast<std::int64_t>(value);
    }
public:
    Value() = default;
    template<class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    Value(T value) : Storage(integral_storage(value)) {}
    Value(BigInt value) : Storage(value_detail::fits_int64(value) ? Storage(value.to_int64()) : Storage(std::move(value))) {}
    template<class T, std::enable_if_t<
        !std::is_integral_v<std::decay_t<T>> || std::is_same_v<std::decay_t<T>, bool>, int> = 0,
        std::enable_if_t<!std::is_same_v<std::decay_t<T>, Value> &&
                         !std::is_same_v<std::decay_t<T>, BigInt> && std::is_constructible_v<Storage, T>, int> = 0>
    Value(T&& value) : Storage(std::forward<T>(value)) {}
    template<class T, class... Args,
             std::enable_if_t<std::is_constructible_v<Storage, std::in_place_type_t<T>, Args...>, int> = 0>
    explicit Value(std::in_place_type_t<T> tag, Args&&... args) : Storage(tag, std::forward<Args>(args)...) {}
    template<std::size_t I, class... Args,
             std::enable_if_t<std::is_constructible_v<Storage, std::in_place_index_t<I>, Args...>, int> = 0>
    explicit Value(std::in_place_index_t<I> tag, Args&&... args) : Storage(tag, std::forward<Args>(args)...) {}
    template<class T, class U, class... Args,
             std::enable_if_t<std::is_constructible_v<Storage, std::in_place_type_t<T>, std::initializer_list<U>&, Args...>, int> = 0>
    explicit Value(std::in_place_type_t<T> tag, std::initializer_list<U> list, Args&&... args)
        : Storage(tag, list, std::forward<Args>(args)...) {}
    template<std::size_t I, class U, class... Args,
             std::enable_if_t<std::is_constructible_v<Storage, std::in_place_index_t<I>, std::initializer_list<U>&, Args...>, int> = 0>
    explicit Value(std::in_place_index_t<I> tag, std::initializer_list<U> list, Args&&... args)
        : Storage(tag, list, std::forward<Args>(args)...) {}

    friend bool operator==(const Value& a, const Value& b) {
        if (a.index() == b.index()) return static_cast<const Storage&>(a) == static_cast<const Storage&>(b);
        const auto* compact = std::get_if<std::int64_t>(&a);
        const auto* wide = std::get_if<BigInt>(&b);
        if (!compact || !wide) {
            compact = std::get_if<std::int64_t>(&b);
            wide = std::get_if<BigInt>(&a);
        }
        return compact && wide && value_detail::fits_int64(*wide) && *compact == wide->to_int64();
    }
    friend bool operator!=(const Value& a, const Value& b) { return !(a == b); }
};

inline ValueRepresentation representation(const Value& value) {
    if (value.valueless_by_exception()) throw std::bad_variant_access();
    return static_cast<ValueRepresentation>(value.index());
}
inline ValueKind kind(const Value& value) {
    switch (representation(value)) {
    case ValueRepresentation::Null: return ValueKind::Null;
    case ValueRepresentation::Boolean: return ValueKind::Boolean;
    case ValueRepresentation::Int64: case ValueRepresentation::BigInt: return ValueKind::Integer;
    case ValueRepresentation::Float: return ValueKind::Float;
    case ValueRepresentation::String: return ValueKind::String;
    case ValueRepresentation::Address: return ValueKind::Address;
    case ValueRepresentation::Object: return ValueKind::Object;
    }
    throw std::bad_variant_access();
}
inline const char* kind_name(ValueKind value) {
    static constexpr const char* names[] = {"null", "boolean", "integer", "float", "string", "address", "object"};
    auto i = static_cast<Index>(value);
    if (i >= 7) throw std::invalid_argument("invalid VM value kind");
    return names[i];
}
inline bool valid_kind(ValueKind value) { return static_cast<Index>(value) < 7; }

class VMError : public std::runtime_error {
public:
    VMError(std::string code, std::string message, CodeAddress position = {})
        : std::runtime_error(std::move(message)), code(std::move(code)), position(position) {}
    std::string code;
    CodeAddress position;
};

template<class T> inline const T& require(const Value& value) {
    auto result = std::get_if<T>(&value);
    if (!result) throw VMError("vm.type", "unexpected runtime value kind");
    return *result;
}

// Use this accessor for integer operations that accept either representation.
inline BigInt require_integer(const Value& value) {
    if (const auto* small = std::get_if<std::int64_t>(&value)) return BigInt(*small);
    return require<BigInt>(value);
}

// Checked narrowing for integrations that require a machine-sized integer.
inline std::int64_t require_integer64(const Value& value) {
    if (const auto* small = std::get_if<std::int64_t>(&value)) return *small;
    const auto& wide = require<BigInt>(value);
    if (!value_detail::fits_int64(wide)) throw VMError("vm.overflow", "integer does not fit int64");
    return wide.to_int64();
}

struct Signature {
    std::vector<ValueKind> parameters;
    ValueKind result = ValueKind::Null; // Null denotes a void result.
    friend bool operator==(const Signature& a, const Signature& b) {
        return a.parameters == b.parameters && a.result == b.result;
    }
};
inline void check_arguments(const Signature& signature, const std::vector<Value>& arguments) {
    if (arguments.size() != signature.parameters.size()) throw VMError("vm.arity", "argument count differs from signature");
    for (Index i = 0; i < arguments.size(); ++i)
        if (kind(arguments[i]) != signature.parameters[i]) throw VMError("vm.type", "argument kind differs from signature");
}
inline void check_result(const Signature& signature, const Value& result) {
    if (kind(result) != signature.result) throw VMError("vm.type", "result kind differs from signature");
}

} // namespace firestone::vm

namespace std {
template<> struct variant_size<firestone::vm::Value> : variant_size<firestone::vm::value_detail::Storage> {};
template<size_t I> struct variant_alternative<I, firestone::vm::Value> :
    variant_alternative<I, firestone::vm::value_detail::Storage> {};
} // namespace std
