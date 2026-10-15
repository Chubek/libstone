#include "Internal.hpp"

namespace gateml {
namespace {
void pattern_names(const Pattern& p, std::set<std::string>& bound) {
    if (p.kind == Pattern::Bind) bound.insert(p.text);
    for (const auto& c : p.children) pattern_names(c, bound);
}
void free_names(const Tree& e, std::set<std::string> bound, std::set<std::string>& free) {
    if (e->kind == Expr::Variable) { if (!bound.contains(e->text)) free.insert(e->text); return; }
    if (e->kind == Expr::Lambda) { bound.insert(e->text); free_names(e->children[0], bound, free); return; }
    if (e->kind == Expr::Let) {
        auto rhs = bound; if (e->recursive) rhs.insert(e->text);
        free_names(e->children[0], rhs, free); bound.insert(e->text);
        free_names(e->children[1], bound, free); return;
    }
    if (e->kind == Expr::Match) {
        free_names(e->children[0], bound, free);
        for (std::size_t i = 0; i < e->patterns.size(); ++i) {
            auto local = bound; pattern_names(e->patterns[i], local);
            free_names(e->children[i + 1], local, free);
        }
        return;
    }
    for (const auto& c : e->children) free_names(c, bound, free);
}
class Lowering {
    std::shared_ptr<Program::Impl> out = std::make_shared<Program::Impl>();
    struct Pending { Tree body; std::size_t instruction; };
    std::vector<Pending> pending;
    const Options& options;
    std::size_t emit(Op op, const Tree& e, std::string text = {}) {
        if (out->code.size() >= options.max_values) throw Error(e->loc, "instruction limit exceeded");
        Instruction i; i.op = op; i.text = std::move(text);
        out->code.code.push_back(std::move(i));
        out->code.source_map.push_back({e->loc.file, static_cast<std::uint32_t>(e->loc.line), static_cast<std::uint32_t>(e->loc.column)});
        return out->code.size() - 1;
    }
    void expression(const Tree& e, std::string self = {}, bool tail = false) {
        auto child = [&](std::size_t n, bool tail_position = false) { expression(e->children[n], {}, tail_position); };
        auto constant = [&](Value v) { out->code.code[emit(Op::Constant, e)].literal = std::move(v); };
        switch (e->kind) {
        case Expr::Integer: constant(value(Value::Kind::Int, e->number)); return;
        case Expr::Boolean: constant(value(Value::Kind::Bool, e->number)); return;
        case Expr::String: constant(value(Value::Kind::String, 0, e->text)); return;
        case Expr::Unit: constant(value(Value::Kind::Unit)); return;
        case Expr::Nil: constant(value(Value::Kind::Nil)); return;
        case Expr::Variable: emit(Op::Load, e, e->text); return;
        case Expr::Lambda: {
            auto pc = emit(Op::Closure, e, e->text);
            auto& i = out->code.code[pc]; i.extra = self;
            std::set<std::string> bound{e->text}; if (!self.empty()) bound.insert(self);
            free_names(e->children[0], bound, i.captures);
            pending.push_back({e->children[0], pc}); return;
        }
        case Expr::Apply: child(0); child(1); emit(tail ? Op::TailApply : Op::Apply, e); return;
        case Expr::Let:
            emit(Op::Enter, e); expression(e->children[0], e->recursive ? e->text : "");
            emit(Op::Bind, e, e->text); child(1, tail); emit(Op::Leave, e); return;
        case Expr::If: {
            child(0); auto no = emit(Op::JumpFalse, e); child(1, tail); auto end = emit(Op::Jump, e);
            out->code.code[no].target = out->code.size(); child(2, tail);
            out->code.code[end].target = out->code.size(); return;
        }
        case Expr::Binary:
            if (e->text == "&&" || e->text == "||") {
                child(0); auto branch = emit(Op::JumpFalse, e);
                if (e->text == "&&") child(1, tail); else constant(value(Value::Kind::Bool, 1));
                auto end = emit(Op::Jump, e); out->code.code[branch].target = out->code.size();
                if (e->text == "&&") constant(value(Value::Kind::Bool, 0)); else child(1, tail);
                out->code.code[end].target = out->code.size(); return;
            }
            child(0); child(1); emit(Op::Binary, e, e->text); return;
        case Expr::Unary: child(0); emit(Op::Unary, e, e->text); return;
        case Expr::Pair: child(0); child(1); emit(Op::Pair, e); return;
        case Expr::Cons: child(0); child(1); emit(Op::Cons, e); return;
        case Expr::Sequence: child(0); emit(Op::Pop, e); child(1, tail); return;
        case Expr::Match: {
            child(0); std::vector<std::size_t> ends;
            for (std::size_t n = 0; n < e->patterns.size(); ++n) {
                auto test = emit(Op::TryMatch, e); out->code.code[test].pattern = e->patterns[n];
                child(n + 1, tail); emit(Op::Leave, e); ends.push_back(emit(Op::Jump, e));
                out->code.code[test].target = out->code.size();
            }
            emit(Op::Fail, e, "non-exhaustive match");
            for (auto pc : ends) out->code.code[pc].target = out->code.size();
            return;
        }
        }
    }
public:
    explicit Lowering(const Options& o) : options(o) {}
    std::shared_ptr<Program::Impl> build(Tree tree) {
        out->tree = tree; expression(tree); emit(Op::Halt, tree);
        for (std::size_t n = 0; n < pending.size(); ++n) {
            auto p = pending[n]; out->code.code[p.instruction].target = out->code.size();
            expression(p.body, {}, true); emit(Op::Return, p.body);
        }
        return out;
    }
};
}
std::shared_ptr<Program::Impl> lower(Tree tree, const Options& options) {
    return Lowering(options).build(std::move(tree));
}
} // namespace gateml
