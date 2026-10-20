#include "internal.hpp"
#include <cmath>
#include <iomanip>
#include <sstream>

namespace flintstone {
std::string format(const Diagnostic& d) {
    std::ostringstream out;
    if (!d.location.file.empty()) {
        out << d.location.file;
        if (d.location.line) out << ':' << d.location.line << ':' << d.location.column;
        out << ": ";
    }
    out << d.code << ": " << d.message;
    if (!d.function.empty()) out << " [@" << d.function << ']';
    if (d.block != invalid_id) out << " [^" << d.block << ']';
    if (d.operation != SIZE_MAX) out << " [operation " << d.operation << ']';
    return out.str();
}
Error::Error(Diagnostics errors)
    : std::runtime_error(errors.empty() ? "Flintstone error" : format(errors.front())), diagnostics_(std::move(errors)) {}
Error::Error(std::string code, std::string message, Location location)
    : Error(Diagnostics{{std::move(code), std::move(message), std::move(location), {}, invalid_id, SIZE_MAX}}) {}
const char* level_name(Level level) {
    switch (level) {
    case Level::High: return "high";
    case Level::Core: return "core";
    case Level::Machine: return "machine";
    }
    detail::fail("level", "invalid IL level");
}
Type Type::void_type() { return {}; }
Type Type::integer(unsigned bits) { Type t; t.kind = TypeKind::Integer; t.bits = bits; return t; }
Type Type::floating(unsigned bits) { Type t; t.kind = TypeKind::Float; t.bits = bits; return t; }
Type Type::index() { Type t; t.kind = TypeKind::Index; return t; }
Type Type::pointer(std::string space) { Type t; t.kind = TypeKind::Pointer; t.name = std::move(space); return t; }
Type Type::vector(std::uint32_t count, Type element) {
    Type t; t.kind = TypeKind::Vector; t.count = count; t.elements.push_back(std::move(element)); return t;
}
Type Type::tuple(std::vector<Type> elements) { Type t; t.kind = TypeKind::Tuple; t.elements = std::move(elements); return t; }
Type Type::opaque(std::string name, std::vector<Type> elements) {
    Type t; t.kind = TypeKind::Opaque; t.name = std::move(name); t.elements = std::move(elements); return t;
}
Type Type::parameter(std::string name) { Type t; t.kind = TypeKind::Parameter; t.name = std::move(name); return t; }
std::string print(const Type& type) {
    std::function<std::string(const Type&, unsigned)> render = [&](const Type& t, unsigned depth) -> std::string {
        detail::check(depth < 64, "type.depth", "type nesting limit exceeded");
        switch (t.kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Integer: return "i" + std::to_string(t.bits);
        case TypeKind::Float: return "f" + std::to_string(t.bits);
        case TypeKind::Index: return "index";
        case TypeKind::Pointer: return "ptr<" + t.name + ">";
        case TypeKind::Parameter: return "param<" + t.name + ">";
        case TypeKind::Vector:
            detail::check(t.elements.size() == 1, "type.vector", "vector requires an element type");
            return "vector<" + std::to_string(t.count) + ", " + render(t.elements[0], depth + 1) + ">";
        case TypeKind::Tuple: case TypeKind::Opaque: {
            std::string out = t.kind == TypeKind::Tuple ? "tuple<" : "opaque<" + t.name;
            for (std::size_t n = 0; n < t.elements.size(); ++n) {
                if (n || t.kind == TypeKind::Opaque) out += ", ";
                out += render(t.elements[n], depth + 1);
            }
            return out + ">";
        }
        }
        detail::fail("type", "invalid type kind");
    };
    return render(type, 0);
}
Layout layout(const Type& type, const Target& target) {
    using detail::check;
    auto add = [&](std::uint64_t a, std::uint64_t b) {
        check(b <= UINT64_MAX - a, "layout.overflow", "type size overflows"); return a + b;
    };
    auto align = [&](std::uint64_t n, std::uint64_t a) { return add(n, a - 1) / a * a; };
    std::function<Layout(const Type&, unsigned)> go = [&](const Type& t, unsigned depth) -> Layout {
        check(depth < 64, "type.depth", "layout nesting limit exceeded");
        if (t.kind == TypeKind::Void) return {};
        if (t.kind == TypeKind::Integer || t.kind == TypeKind::Float || t.kind == TypeKind::Index || t.kind == TypeKind::Pointer) {
            auto bits = t.kind == TypeKind::Index ? target.index_bits : t.kind == TypeKind::Pointer ? target.pointer_bits : t.bits;
            check(bits > 0 && bits <= 65536, "layout.width", "invalid scalar width");
            std::uint64_t size = (bits + 7) / 8, alignment = 1;
            // Flintstone's portable layout convention, not an inferred native ABI.
            while (alignment < size && alignment < 8) alignment *= 2;
            return {size, alignment};
        }
        if (t.kind == TypeKind::Vector) {
            check(t.elements.size() == 1 && t.count, "layout.vector", "invalid vector shape");
            auto element = go(t.elements[0], depth + 1);
            check(element.size && element.size <= UINT64_MAX / t.count, "layout.overflow", "invalid or oversized vector");
            return {element.size * t.count, element.alignment};
        }
        if (t.kind == TypeKind::Tuple) {
            Layout result;
            for (const auto& field : t.elements) {
                auto f = go(field, depth + 1);
                check(f.size != 0, "layout.field", "void or empty fields have no storage layout");
                result.size = add(align(result.size, f.alignment), f.size);
                result.alignment = std::max(result.alignment, f.alignment);
            }
            result.size = align(result.size, result.alignment); return result;
        }
        detail::fail("layout.unsupported", "opaque/parameter type needs an explicit target layout adapter");
    };
    return go(type, 0);
}
Operand Operand::ref(ValueId id) { Operand o; o.value = id; return o; }
Operand Operand::constant(std::int64_t n, Type type) {
    Operand o; o.kind = OperandKind::Integer; o.integer = n; o.type = std::move(type); return o;
}
Operand Operand::real(double n, Type type) {
    Operand o; o.kind = OperandKind::Float; o.floating = n; o.type = std::move(type); return o;
}
Operand Operand::named(std::string symbol, Type type) {
    Operand o; o.kind = OperandKind::Symbol; o.symbol = std::move(symbol); o.type = std::move(type); return o;
}
ValueId Function::fresh_value() const {
    ValueId next = 0;
    auto account = [&](ValueId id) {
        detail::check(id < invalid_id - 1, "id.exhausted", "value ID space exhausted"); next = std::max(next, id + 1);
    };
    for (const auto& p : parameters) account(p.id);
    for (const auto& b : blocks) for (const auto& op : b.operations) for (const auto& r : op.results) account(r.id);
    return next;
}
BlockId Function::fresh_block() const {
    BlockId next = 0;
    for (const auto& b : blocks) {
        detail::check(b.id < invalid_id - 1, "id.exhausted", "block ID space exhausted"); next = std::max(next, b.id + 1);
    }
    return next;
}
Block& Function::block(BlockId id) {
    for (auto& b : blocks) if (b.id == id) return b;
    detail::fail("block.unknown", "unknown block ^" + std::to_string(id));
}
const Block& Function::block(BlockId id) const { return const_cast<Function*>(this)->block(id); }
Function& Module::function(const std::string& name) {
    for (auto& f : functions) if (f.name == name) return f;
    detail::fail("symbol.unknown", "unknown function @" + name);
}
const Function& Module::function(const std::string& name) const { return const_cast<Module*>(this)->function(name); }
Value Builder::parameter(Type t) {
    Value value{function_.fresh_value(), std::move(t), {}}; function_.parameters.push_back(value); return value;
}
BlockId Builder::block(std::string name) {
    auto id = function_.fresh_block();
    function_.blocks.push_back({id, std::move(name), {}, {}});
    if (function_.entry == invalid_id) function_.entry = id;
    return id;
}
Value Builder::emit(BlockId block, std::string name, Type type, std::vector<Operand> inputs, Attributes attrs) {
    Value value{function_.fresh_value(), std::move(type), {}};
    function_.block(block).operations.push_back({std::move(name), {value}, std::move(inputs), {}, {}, std::move(attrs), {}});
    return value;
}
void Builder::effect(BlockId block, std::string name, std::vector<Operand> inputs, Attributes attrs) {
    function_.block(block).operations.push_back({std::move(name), {}, std::move(inputs), {}, {}, std::move(attrs), {}});
}
void Builder::jump(BlockId from, BlockId to) { Operation op; op.name = "cf.jump"; op.successors = {to}; function_.block(from).operations.push_back(op); }
void Builder::branch(BlockId from, Operand condition, BlockId yes, BlockId no) {
    Operation op; op.name = "cf.branch"; op.operands = {std::move(condition)}; op.successors = {yes, no}; function_.block(from).operations.push_back(op);
}
void Builder::return_value(BlockId block, std::optional<Operand> value) { effect(block, "func.return", value ? std::vector<Operand>{*value} : std::vector<Operand>{}); }
Type OperationContext::type(const Operand& op) const {
    if (op.kind != OperandKind::Value) return detail::resolve(op.type, module);
    auto found = values.find(op.value);
    detail::check(found != values.end(), "ssa.undefined", "undefined value %" + std::to_string(op.value));
    return found->second;
}
std::string version() { return "0.1.0"; }

namespace detail {
bool valid_name(std::string_view s) {
    if (s.empty()) return false;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!alpha(s.front())) return false;
    for (char c : s) if (!alpha(c) && !(c >= '0' && c <= '9') && c != '.' && c != '-') return false;
    return true;
}
std::string dialect_name(const std::string& name) {
    auto dot = name.find('.');
    check(dot != std::string::npos && dot && dot + 1 < name.size(), "dialect.name", "operation/type must have a dialect-qualified name: " + name);
    return name.substr(0, dot);
}
namespace {
Type resolve_bounded(const Type& type, const Module& module, std::size_t& remaining) {
    std::set<std::string> active;
    std::function<Type(const Type&, unsigned)> go = [&](const Type& t, unsigned depth) -> Type {
        check(depth < 64, "type.depth", "type substitution nesting limit exceeded");
        check(remaining > 0, "type.limit", "type expansion node budget exhausted");
        --remaining;
        if (t.kind == TypeKind::Parameter) {
            check(!t.bits && !t.count && t.elements.empty() && valid_name(t.name), "type.parameter", "malformed type parameter");
            auto found = module.type_bindings.find(t.name);
            check(found != module.type_bindings.end(), "type.unbound", "unbound type parameter " + t.name);
            check(active.insert(t.name).second, "type.cycle", "cyclic type parameter " + t.name);
            auto result = go(found->second, depth + 1); active.erase(t.name); return result;
        }
        if (t.kind == TypeKind::Index) {
            check(!t.bits && !t.count && t.elements.empty() && t.name.empty(), "type.index", "malformed index type");
            return Type::integer(module.target.index_bits);
        }
        // Do not copy the entire subtree before accounting for expansion. Small
        // alias DAGs can otherwise produce exponentially large owning types.
        Type result; result.kind = t.kind; result.bits = t.bits; result.count = t.count; result.name = t.name;
        check(t.elements.size() <= remaining, "type.limit", "type expansion node budget exhausted");
        result.elements.reserve(t.elements.size());
        for (const auto& element : t.elements) result.elements.push_back(go(element, depth + 1));
        return result;
    };
    return go(type, 0);
}
}
Type resolve(const Type& type, const Module& module) {
    std::size_t remaining = 1000000; return resolve_bounded(type, module, remaining);
}
Module specialize(const Module& source) {
    Module result = source;
    std::size_t remaining = 1000000;
    auto concrete = [&](const Type& type) { return resolve_bounded(type, source, remaining); };
    for (auto& [name, type] : result.type_bindings) type = concrete(type);
    auto attrs = [&](Attributes& attributes) {
        for (auto& [name, value] : attributes) if (auto t = std::get_if<Type>(&value)) *t = concrete(*t);
    };
    attrs(result.attributes); attrs(result.target.parameters);
    for (auto& [name, parameters] : result.dialects) attrs(parameters);
    for (auto& f : result.functions) {
        f.result = concrete(f.result); attrs(f.attributes);
        for (auto& p : f.parameters) p.type = concrete(p.type);
        for (auto& b : f.blocks) for (auto& op : b.operations) {
            attrs(op.attributes);
            for (auto& r : op.results) r.type = concrete(r.type);
            for (auto& o : op.operands) if (o.kind != OperandKind::Value) o.type = concrete(o.type);
        }
    }
    return result;
}
}
} // namespace flintstone
