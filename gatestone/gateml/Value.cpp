#include "Internal.hpp"
#include <iomanip>
#include <sstream>

namespace gateml {
namespace {
// Deferred destruction avoids C++ stack overflow for long immutable lists and
// closure chains. No global ownership or tracing collector is needed.
std::shared_ptr<Value::Data> data() {
    return {new Value::Data, [](Value::Data* p) {
        static thread_local std::vector<Value::Data*> pending;
        static thread_local bool draining = false;
        pending.push_back(p);
        if (draining) return;
        draining = true;
        while (!pending.empty()) { auto* q = pending.back(); pending.pop_back(); delete q; }
        draining = false;
    }};
}
}
Value::Value() : data(gateml::value(Kind::Unit).data) {}
Value::Value(std::shared_ptr<const Data> d) : data(std::move(d)) {
    if (!data) throw std::invalid_argument("null GateML value");
}
Value::Kind Value::kind() const { return data->kind; }
std::int64_t Value::integer() const {
    if (kind() != Kind::Int) throw std::runtime_error("expected int");
    return data->number;
}
bool Value::boolean() const {
    if (kind() != Kind::Bool) throw std::runtime_error("expected bool");
    return data->number != 0;
}
const std::string& Value::string() const {
    if (kind() != Kind::String) throw std::runtime_error("expected string");
    return data->text;
}
const std::vector<Value>& Value::fields() const { return data->fields; }
Value value(Value::Kind kind, std::int64_t number, std::string text, std::vector<Value> fields) {
    auto d = data(); d->kind = kind; d->number = number; d->text = std::move(text); d->fields = std::move(fields);
    return Value(std::move(d));
}
Value function(Closure closure) {
    auto d = data(); d->kind = Value::Kind::Function; d->closure = std::make_shared<Closure>(std::move(closure));
    return Value(std::move(d));
}
std::string Value::show(std::size_t max_depth, std::size_t max_bytes) const {
    max_depth = std::min(max_depth, std::size_t{256});
    std::string out; bool truncated = false;
    auto append = [&](std::string_view s) {
        auto remaining = max_bytes - out.size();
        if (s.size() > remaining) { out.append(s.substr(0, remaining)); truncated = true; }
        else out.append(s);
    };
    std::function<void(const Value&, std::size_t)> render = [&](const Value& v, std::size_t depth) {
        if (out.size() >= max_bytes) { truncated = true; return; }
        if (depth > max_depth) { append("..."); return; }
        switch (v.kind()) {
        case Kind::Unit: append("()"); return;
        case Kind::Int: append(std::to_string(v.integer())); return;
        case Kind::Bool: append(v.boolean() ? "true" : "false"); return;
        case Kind::String: {
            append("\"");
            for (char c : v.string()) {
                if (out.size() >= max_bytes) { truncated = true; break; }
                switch (c) {
                case '\n': append("\\n"); break; case '\r': append("\\r"); break; case '\t': append("\\t"); break;
                case '\\': append("\\\\"); break; case '"': append("\\\""); break; default: append(std::string_view(&c, 1));
                }
            }
            append("\""); return;
        }
        case Kind::Nil: append("[]"); return;
        case Kind::Cons: {
            append("["); Value current = v; std::size_t n = 0;
            while (current.kind() == Kind::Cons && n < max_depth && out.size() < max_bytes) {
                if (n++) append("; ");
                render(current.fields()[0], depth + 1); current = current.fields()[1];
            }
            if (current.kind() != Kind::Nil) append("; ...");
            append("]"); return;
        }
        case Kind::Pair:
            append("("); render(v.fields()[0], depth + 1); append(", "); render(v.fields()[1], depth + 1); append(")"); return;
        case Kind::None: append("None"); return;
        case Kind::Some: case Kind::Ok: case Kind::Error:
            append(v.kind() == Kind::Some ? "Some (" : v.kind() == Kind::Ok ? "Ok (" : "Error (");
            render(v.fields()[0], depth + 1); append(")"); return;
        case Kind::Function: append("<fun>"); return;
        }
    };
    render(*this, 0);
    // Mark truncation in place so the byte budget is a hard ceiling, without
    // appending "..." to a value that may already end with one.
    if (truncated && out.size() >= 3 && !out.ends_with("..."))
        out.replace(out.size() - 3, 3, "...");
    return out;
}
bool equal(const Value& a, const Value& b, std::size_t) {
    std::vector<std::pair<Value, Value>> work{{a, b}};
    std::size_t steps = 0;
    while (!work.empty()) {
        if (++steps > 1'000'000) throw std::runtime_error("equality work limit exceeded");
        auto [x, y] = std::move(work.back()); work.pop_back();
        if (x.kind() == Value::Kind::Function || y.kind() == Value::Kind::Function)
            throw std::runtime_error("functions do not support equality");
        if (x.kind() != y.kind() || x.data->number != y.data->number || x.data->text != y.data->text) return false;
        for (std::size_t i = 0; i < x.fields().size(); ++i) work.emplace_back(x.fields()[i], y.fields()[i]);
    }
    return true;
}
bool match(const Pattern& p, const Value& v, Env& bindings) {
    using K = Value::Kind;
    switch (p.kind) {
    case Pattern::Wild: return true;
    case Pattern::Bind: bindings[p.text] = v; return true;
    case Pattern::Integer: return v.kind() == K::Int && v.integer() == p.number;
    case Pattern::Boolean: return v.kind() == K::Bool && v.boolean() == (p.number != 0);
    case Pattern::Unit: return v.kind() == K::Unit;
    case Pattern::Nil: return v.kind() == K::Nil;
    case Pattern::None: return v.kind() == K::None;
    default: break;
    }
    const auto expected = p.kind == Pattern::Cons ? K::Cons : p.kind == Pattern::Pair ? K::Pair :
                          p.kind == Pattern::Some ? K::Some : p.kind == Pattern::Ok ? K::Ok : K::Error;
    if (v.kind() != expected) return false;
    for (std::size_t i = 0; i < p.children.size(); ++i)
        if (!match(p.children[i], v.fields()[i], bindings)) return false;
    return true;
}
Value binary(const std::string& op, const Value& a, const Value& b) {
    using K = Value::Kind;
    if (op == "=" || op == "<>") return value(K::Bool, equal(a, b) == (op == "="));
    if (op == "^") return value(K::String, 0, a.string() + b.string());
    if (op == "&&") return value(K::Bool, a.boolean() && b.boolean());
    if (op == "||") return value(K::Bool, a.boolean() || b.boolean());
    if (op == "<") return value(K::Bool, a.integer() < b.integer());
    if (op == ">") return value(K::Bool, a.integer() > b.integer());
    if (op == "<=") return value(K::Bool, a.integer() <= b.integer());
    if (op == ">=") return value(K::Bool, a.integer() >= b.integer());
    // Reuse MetaIL's verified integer semantics; reject unknown names loudly
    // rather than silently falling back to a default opcode.
    static const std::map<std::string, metail::Op> codes = {
        {"+", metail::Op::Add}, {"-", metail::Op::Sub}, {"*", metail::Op::Mul},
        {"/", metail::Op::Div}, {"mod", metail::Op::Mod}
    };
    auto code = codes.find(op);
    if (code == codes.end()) throw std::runtime_error("unknown binary operator: " + op);
    auto result = metail::evaluate(code->second, a.integer(), b.integer());
    if (!result) throw std::runtime_error("invalid division or remainder");
    return value(K::Int, *result);
}
Value unary(const std::string& op, const Value& v) {
    if (op != "not" && op != "-") throw std::runtime_error("unknown unary operator: " + op);
    if (op == "not") return value(Value::Kind::Bool, !v.boolean());
    return value(Value::Kind::Int, *metail::evaluate(metail::Op::Sub, 0, v.integer()));
}
} // namespace gateml
