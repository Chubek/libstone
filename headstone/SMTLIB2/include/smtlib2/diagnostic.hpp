#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace smtlib2 {
struct Position { std::size_t offset = 0, line = 1, column = 1; };
struct Span { Position begin, end; };
enum class ErrorCategory { unspecified, lexical, syntax, resource_limit };
class Error : public std::runtime_error {
public:
    Error(Span span, std::string message, ErrorCategory category = ErrorCategory::unspecified)
        : std::runtime_error(std::move(message)), span_(span), category_(category) {}
    const Span& span() const noexcept { return span_; }
    ErrorCategory category() const noexcept { return category_; }
private:
    Span span_;
    ErrorCategory category_;
};
/// @implstring Diagnostics should eventually retain a source identifier and
/// related spans, and support recovery without concealing errors.
}
