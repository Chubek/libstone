#include "Internal.hpp"
#include <charconv>

namespace gateml {
// The initial FFI is deliberately closed: only these typed, deterministic host
// primitives are exposed. Programs cannot access files, processes or pointers.
Value call_builtin(const std::string& name, const std::vector<Value>& a, Runtime& r) {
    using K = Value::Kind;
    // Operator values are ordinary strict functions; infix boolean syntax is
    // lowered separately to short-circuit branches.
    if (name.starts_with("#operator:")) return r.binary_result(name.substr(std::string_view("#operator:").size()), a[0], a[1]);
    if (name.starts_with("#unary:")) return unary(name.substr(std::string_view("#unary:").size()), a[0]);
    if (name == "Some" || name == "Ok" || name == "Error")
        return value(name == "Some" ? K::Some : name == "Ok" ? K::Ok : K::Error, 0, {}, {a[0]});
    if (name == "fst" || name == "snd") return a[0].fields()[name == "fst" ? 0 : 1];
    if (name == "failwith") throw std::runtime_error(a[0].string());
    if (name == "print_int" || name == "println_int") {
        r.write(std::to_string(a[0].integer())); if (name == "println_int") r.write("\n");
        return value(K::Unit);
    }
    if (name == "print_string" || name == "print_endline") {
        r.write(a[0].string()); if (name == "print_endline") r.write("\n"); return value(K::Unit);
    }
    if (name == "Int.to_string" || name == "String.of_int") return value(K::String, 0, std::to_string(a[0].integer()));
    if (name == "Int.of_string") {
        const auto& s = a[0].string(); std::int64_t n = 0;
        auto parsed = std::from_chars(s.data(), s.data() + s.size(), n);
        if (parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size()) return value(K::None);
        return value(K::Some, 0, {}, {value(K::Int, n)});
    }
    if (name == "String.length") return value(K::Int, static_cast<std::int64_t>(a[0].string().size()));
    if (name == "String.compare") {
        int c = a[0].string().compare(a[1].string()); return value(K::Int, c < 0 ? -1 : c > 0 ? 1 : 0);
    }
    if (name == "String.get" || name == "String.sub") {
        const auto& s = a[0].string(); auto start = a[1].integer();
        auto length = name == "String.get" ? 1 : a[2].integer();
        if (start < 0 || length < 0 || static_cast<std::uint64_t>(start) > s.size() ||
            static_cast<std::uint64_t>(length) > s.size() - static_cast<std::size_t>(start))
            throw std::runtime_error("string index out of bounds");
        return value(K::String, 0, s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(length)));
    }
    static const std::map<std::string, metail::Op> bitwise = {
        {"Int.logand", metail::Op::And}, {"Int.logor", metail::Op::Or},
        {"Int.logxor", metail::Op::Xor}, {"Int.shift_left", metail::Op::Shl},
        {"Int.shift_right", metail::Op::Shr}
    };
    auto op = bitwise.find(name);
    if (op == bitwise.end()) throw std::runtime_error("unknown builtin: " + name);
    auto n = metail::evaluate(op->second, a[0].integer(), a[1].integer());
    if (!n) throw std::runtime_error("shift count must be in [0, 63]");
    return value(K::Int, *n);
}
} // namespace gateml
