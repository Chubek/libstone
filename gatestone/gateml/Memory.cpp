#include "Internal.hpp"

namespace gateml {
void Runtime::account() {
    if (++allocations > options.max_values) throw std::runtime_error("value allocation limit exceeded");
    if (stack.size() + frames.size() + scopes.size() > options.max_values)
        throw std::runtime_error("runtime stack limit exceeded");
}
void Runtime::write(std::string_view text) {
    if (text.size() > options.max_output_bytes - output.size()) throw std::runtime_error("output limit exceeded");
    output.append(text);
}
void Runtime::push_result(Value result) {
    if (result.kind() == Value::Kind::String) {
        const auto bytes = result.string().size();
        if (bytes > options.max_string_bytes - string_bytes)
            throw std::runtime_error("string allocation limit exceeded");
        string_bytes += bytes;
    }
    stack.push_back(std::move(result));
}
Value Runtime::binary_result(const std::string& op, const Value& a, const Value& b) const {
    if (op == "^") {
        const auto left = a.string().size(), right = b.string().size();
        if (left > options.max_source_bytes || right > options.max_source_bytes - left)
            throw std::runtime_error("string size limit exceeded");
        // Check before allocating, for both infix and first-class concatenation.
        const auto remaining = options.max_string_bytes - string_bytes;
        if (left > remaining || right > remaining - left)
            throw std::runtime_error("string allocation limit exceeded");
    }
    return binary(op, a, b);
}
Value Runtime::pop() {
    if (stack.empty()) throw std::runtime_error("bytecode stack underflow");
    auto v = stack.back(); stack.pop_back(); return v;
}
} // namespace gateml
