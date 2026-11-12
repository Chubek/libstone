#pragma once
#include "Core.hpp"
#include <variant>

namespace moonstone::inline v1 {

enum class ErrorCategory { InvalidInput, Unsupported, BudgetExhausted, NotFound, Internal };

struct ErrorContext {
    std::string operation;
    std::optional<SourceSpan> source;
};

struct Error {
    ErrorCategory category = ErrorCategory::Internal;
    std::string message;
    std::vector<ErrorContext> contexts;
};

// Accessing the wrong alternative throws; there is no default successful result.
template <class T> class Result {
    std::variant<T, Error> storage_;

  public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error error) : storage_(std::move(error)) {}
    bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
    explicit operator bool() const noexcept { return has_value(); }
    T &value() { return std::get<T>(storage_); }
    const T &value() const { return std::get<T>(storage_); }
    Error &error() { return std::get<Error>(storage_); }
    const Error &error() const { return std::get<Error>(storage_); }
};

template <> class Result<void> {
    std::optional<Error> error_;

  public:
    Result() = default;
    Result(Error error) : error_(std::move(error)) {}
    bool has_value() const noexcept { return !error_; }
    explicit operator bool() const noexcept { return has_value(); }
    void value() const {
        if (error_)
            throw std::bad_variant_access();
    }
    Error &error() { return error_.value(); }
    const Error &error() const { return error_.value(); }
};

} // namespace moonstone::inline v1
