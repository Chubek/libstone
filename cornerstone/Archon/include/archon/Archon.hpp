#ifndef ARCHON_EMBEDDING_HPP
#define ARCHON_EMBEDDING_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

struct _object;      // PyObject, without a public dependency on Python.h
struct _ts;          // PyThreadState

namespace archon {

class Error : public std::runtime_error {
public:
    Error(std::string message, std::string python_type = {});
    // Empty for C++-side failures. Python exceptions are consumed: what()
    // contains their message (including any Archon source location).
    const std::string& python_type() const noexcept { return python_type_; }

private:
    std::string python_type_;
};

// Optional owner of a new CPython main interpreter. Construct and destroy on
// the same thread, once per process; destroy all Values and join all threads
// using Archon first. No GIL is held between facade calls. If the application
// already owns CPython, do not construct this class: call the facade directly.
// A host holding the GIL must release it before blocking on facade workers.
// Only CPython's main interpreter and GIL-enabled builds are supported.
class Interpreter {
public:
    explicit Interpreter(std::string_view package_root = {});
    ~Interpreter() noexcept;
    Interpreter(const Interpreter&) = delete;
    Interpreter& operator=(const Interpreter&) = delete;

private:
    _ts* state_;
};

// Add the trusted directory containing the "archon" Python package to the
// front of sys.path. Configure the import path before the first API call, and
// keep the interpreter alive until all results are destroyed.
void add_package_path(std::string_view package_root);

namespace detail { struct Access; }

// An owning handle. Copies alias the same Python object, not a deep copy.
// Each operation and reference release acquires the main interpreter's GIL.
// Do not access the same C++ handle concurrently with assignment/destruction,
// or race Python inputs with caller mutations. Default/moved-from equals None.
class Value {
public:
    using List = std::vector<Value>;
    using Object = std::map<std::string, Value>;
    using Bytes = std::vector<std::uint8_t>;

    Value() noexcept = default;
    Value(bool value);
    Value(double value);
    Value(const char* value);
    Value(std::string_view value);
    Value(const std::string& value) : Value(std::string_view(value)) {}
    Value(const List& value);
    Value(const Object& value);

    template <class Integer, std::enable_if_t<
        std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>
        && (sizeof(Integer) <= sizeof(std::uint64_t)), int> = 0>
    Value(Integer value) {
        if constexpr (std::is_signed_v<Integer>)
            *this = from_signed(static_cast<std::int64_t>(value));
        else
            *this = from_unsigned(static_cast<std::uint64_t>(value));
    }

    Value(const Value& other);
    Value(Value&& other) noexcept;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept;
    ~Value() noexcept;

    static Value from_bytes(const Bytes& value);
    static Value integer(std::string_view decimal);

    bool is_none() const;
    bool as_bool() const;
    std::int64_t as_int64() const;    // strict, with range checking
    std::uint64_t as_uint64() const;
    std::string as_integer_string() const; // lossless, including >64 bits
    double as_double() const;
    std::string as_string() const;    // Unicode to UTF-8 (not Python str(value))
    std::string as_path() const;      // str/bytes/path-like, in Python's filesystem encoding
    Bytes as_bytes() const;         // bytes or bytearray
    List as_list() const;            // list or tuple, elements remain handles
    Object as_object() const;        // dict with string keys, values remain handles
    std::size_t size() const;
    Value at(std::string_view key) const;
    Value at(std::size_t index) const;
    Value attribute(std::string_view name) const;  // public result attributes
    Value call(const List& args = {}, const Object& kwargs = {}) const;
    void swap(Value& other) noexcept;

private:
    friend struct detail::Access;
    _object* object_ = nullptr;
    static Value from_signed(std::int64_t value);
    static Value from_unsigned(std::uint64_t value);
};

using List = Value::List;
using Object = Value::Object;
using Bytes = Value::Bytes;

// Generic access is restricted to the documented archon.api export table.
Value get_export(std::string_view name);
Value dispatch(std::string_view name, const List& args = {}, const Object& kwargs = {});
std::vector<std::string> exports();

namespace detail {
inline std::vector<std::string> strings(const Value& value) {
    std::vector<std::string> result;
    for (const auto& item : value.as_list())
        result.push_back(item.as_string());
    return result;
}
}

inline Value parse_source(std::string_view source, std::string_view filename = "<source>") {
    return dispatch("parse_source", {source, filename});
}

// Compilation and validation options are "parameters" (dict) and "features"
// (list or None to use defaults). ArchDB results are structured Value dicts.
inline Value compile_file(std::string_view path, const Object& options = {}) {
    return dispatch("compile_file", {path}, options);
}
inline void validate_source(std::string_view path, const Object& options = {}) {
    dispatch("validate_source", {path}, options);
}
inline Value load_database(std::string_view path) {
    return dispatch("load_database", {path});
}
inline void save_database(const Value& database, std::string_view path) {
    dispatch("save_database", {database, path});
}
inline Value parse_database(std::string_view text, std::string_view source = "<ArchDB>") {
    return dispatch("parse_database", {text}, {{"source", source}});
}
inline std::string dump_database(const Value& database) {
    return dispatch("dump_database", {database}).as_string();
}
inline Value inspect_database(const Value& database) {
    return dispatch("inspect_database", {database});
}
inline void validate_database(const Value& database) {
    dispatch("validate_database", {database});
}
inline std::string generate_codec(const Value& database, std::string_view prefix = "archon") {
    return dispatch("generate_codec", {database, prefix}).as_string();
}
inline std::string generate_emulator(const Value& database, std::string_view prefix = "archon") {
    return dispatch("generate_emulator", {database, prefix}).as_string();
}
inline std::string generate_emulator_header(const Value& database,
                                             std::string_view prefix = "archon") {
    return dispatch("generate_emulator_header", {database, prefix}).as_string();
}

struct Instruction {
    std::string name;
    Object operands;
};

class Target;

class MachineState {
public:
    List registers() const { return value_.attribute("registers").as_list(); }
    Object scalars() const { return value_.attribute("scalars").as_object(); }
    Object stacks() const { return value_.attribute("stacks").as_object(); }
    Object memories() const { return value_.attribute("memories").as_object(); }
    MachineState copy() const { return MachineState(value_.attribute("copy").call()); }
    const Value& value() const noexcept { return value_; }

private:
    friend class Target;
    explicit MachineState(Value value);
    Value value_;
};

class Target {
public:
    explicit Target(const Value& database) : value_(dispatch("Target", {database})) {}

    Instruction assemble(std::string_view assembly) const {
        return instruction(method("assemble", {assembly}));
    }
    Bytes encode(const Instruction& inst) const {
        return method("encode", {inst.name, inst.operands}).as_bytes();
    }
    Instruction decode(const Bytes& data) const {
        return instruction(method("decode", {Value::from_bytes(data)}));
    }
    std::string disassemble(const Bytes& data) const {
        return method("disassemble", {Value::from_bytes(data)}).as_string();
    }
    Bytes assemble_program(std::string_view source, const Value& base_address = 0,
                           std::string_view filename = "<assembly>") const {
        return method("assemble_program", {source},
                      {{"base_address", base_address}, {"filename", filename}}).as_bytes();
    }
    std::string disassemble_program(const Bytes& data, const Value& base_address = 0) const {
        return method("disassemble_program", {Value::from_bytes(data)},
                      {{"base_address", base_address}}).as_string();
    }

    // Validated optional overrides: registers (list), scalars (dict), stacks
    // (dict of lists), and memories (dict of bytes).
    MachineState new_state(const Object& overrides = {}) const {
        return MachineState(method("new_state", {}, overrides));
    }
    MachineState execute_state(const Instruction& inst, const MachineState& state) const {
        return MachineState(method("execute_state", {inst.name, inst.operands, state.value()}));
    }
    MachineState execute_program_state(const Bytes& data, const MachineState& state,
                                       const Value& base_address = 0) const {
        return MachineState(method("execute_program_state", {Value::from_bytes(data), state.value()},
                                   {{"base_address", base_address}}));
    }
    List execute(const Instruction& inst, const List& registers) const {
        return method("execute", {inst.name, inst.operands, registers}).as_list();
    }
    List execute_program(const Bytes& data, const List& registers,
                         const Value& base_address = 0) const {
        return method("execute_program", {Value::from_bytes(data), registers},
                      {{"base_address", base_address}}).as_list();
    }
    const Value& value() const noexcept { return value_; }

private:
    Value method(std::string_view name, const List& args, const Object& kwargs = {}) const {
        return value_.attribute(name).call(args, kwargs);
    }
    static Instruction instruction(const Value& value) {
        return {value.at(0).as_string(), value.at(1).as_object()};
    }
    Value value_;
};

} // namespace archon

#endif
