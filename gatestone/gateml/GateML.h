#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace gateml {
struct Location {
    std::string file;
    std::size_t line = 1, column = 1;
};
class Error : public std::runtime_error {
public:
    Location location;
    Error(Location location, std::string message);
};
struct Options {
    bool standard_library = true;
    bool optimize = true;
    std::size_t max_source_bytes = 4 * 1024 * 1024;
    std::size_t max_depth = 256;
    std::size_t max_steps = 1'000'000;
    std::size_t max_values = 1'000'000;
    std::size_t max_string_bytes = 16 * 1024 * 1024;
    std::size_t max_output_bytes = 1024 * 1024;
};
// Immutable, owned values. Function values remain valid after execution ends.
class Value {
public:
    enum class Kind { Unit, Int, Bool, String, Nil, Cons, Pair, None, Some, Ok, Error, Function };
    struct Data;
    std::shared_ptr<const Data> data;
    Value();
    explicit Value(std::shared_ptr<const Data> data);
    Kind kind() const;
    std::int64_t integer() const;
    bool boolean() const;
    const std::string& string() const;
    const std::vector<Value>& fields() const;
    std::string show(std::size_t max_depth = 128, std::size_t max_bytes = 65536) const;
};
struct Program {
    struct Impl;
    std::shared_ptr<const Impl> impl;
    std::string type;
};
struct Execution {
    Value value;
    std::string output;
    std::size_t steps = 0;
};
Program compile(std::string_view source, std::string file = "<input>", Options options = {});
Execution run(const Program& program, Options options = {});
Execution evaluate(std::string_view source, Options options = {});
std::string disassemble(const Program& program);
// Native backend accepts the closed scalar subset, and diagnoses other values.
std::vector<std::uint8_t> executable(std::string_view source, Options options = {});
std::string_view standard_library();
std::string read_file(const std::string& path, std::size_t limit = 4 * 1024 * 1024);
int cli(int argc, char** argv);
} // namespace gateml
