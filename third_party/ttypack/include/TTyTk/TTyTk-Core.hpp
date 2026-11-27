#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__linux__)
#define TTYTK_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#define TTYTK_PLATFORM_MACOS 1
#else
#error "not implemented on this platform"
#endif

namespace ttytk {

/** Return the library version as major, minor, and patch components. */
[[nodiscard]] constexpr auto version() noexcept -> std::array<unsigned, 3> {
    return {0U, 1U, 0U};
}

/** An operation failure with a numeric code and diagnostic message. */
struct Error {
    int code{};
    std::string message{};

    [[nodiscard]] friend auto operator==(const Error&, const Error&) -> bool =
        default;
};

/** Value-or-error storage for operations that return a value. */
template <typename T>
class Result {
public:
    Result(const T& value) : storage_(value) {}
    Result(T&& value) : storage_(std::move(value)) {}
    Result(const Error& error) : storage_(error) {}
    Result(Error&& error) : storage_(std::move(error)) {}

    [[nodiscard]] auto has_value() const noexcept -> bool {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] auto value() & -> T& {
        return std::get<T>(storage_);
    }

    [[nodiscard]] auto value() const& -> const T& {
        return std::get<T>(storage_);
    }

    [[nodiscard]] auto value() && -> T&& {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] auto error() & -> Error& {
        return std::get<Error>(storage_);
    }

    [[nodiscard]] auto error() const& -> const Error& {
        return std::get<Error>(storage_);
    }

private:
    std::variant<T, Error> storage_;
};

/** Value-or-error storage for operations with no success value. */
template <>
class Result<void> {
public:
    Result() = default;
    Result(const Error& error) : error_(error) {}
    Result(Error&& error) : error_(std::move(error)) {}

    [[nodiscard]] auto has_value() const noexcept -> bool {
        return !error_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    /** Check that the operation succeeded without retrieving a value. */
    void value() const {
        if (!has_value()) throw std::bad_variant_access{};
    }

    [[nodiscard]] auto error() & -> Error& {
        return error_.value();
    }

    [[nodiscard]] auto error() const& -> const Error& {
        return error_.value();
    }

private:
    std::optional<Error> error_;
};

/** A non-owning view of contiguous objects. */
template <typename T>
using Span = std::span<T>;

/** An owning byte buffer. */
using Bytes = std::vector<std::byte>;

namespace detail {

struct Utf8Scalar {
    char32_t value;
    std::size_t length;
    bool valid;
};

// Consume the longest prefix that can begin a well-formed UTF-8 scalar
// (Unicode maximal subpart). All callers share the same recovery boundaries.
[[nodiscard]] constexpr auto decode_utf8_scalar(const std::string_view input,
                                               const std::size_t offset) noexcept -> Utf8Scalar {
    if (offset >= input.size()) return {U'\uFFFD', 0, false};
    const auto lead = static_cast<unsigned char>(input[offset]);
    if (lead < 0x80) return {lead, 1, true};

    unsigned length = 0;
    char32_t value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1F;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07;
    } else {
        return {U'\uFFFD', 1, false};
    }

    for (std::size_t index = 1; index < length; ++index) {
        if (index >= input.size() - offset) return {U'\uFFFD', index, false};
        const auto byte = static_cast<unsigned char>(input[offset + index]);
        if (byte < 0x80 || byte > 0xBF ||
            (index == 1 && ((lead == 0xE0 && byte < 0xA0) || (lead == 0xED && byte > 0x9F) ||
                            (lead == 0xF0 && byte < 0x90) || (lead == 0xF4 && byte > 0x8F)))) {
            return {U'\uFFFD', index, false};
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    return {value, length, true};
}

}  // namespace detail

}  // namespace ttytk
