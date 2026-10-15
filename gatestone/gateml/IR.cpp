#include "Internal.hpp"
#include <sstream>

namespace gateml {
namespace {
struct ISA {
    static constexpr std::size_t register_count = 1;
    template<class VM> static bool execute(VM& vm, const Instruction& i) {
        auto& r = *static_cast<Runtime*>(vm.reg(0).data.pointer);
        r.account();
        const auto* loc = vm.program().source_at(vm.pc());
        if (loc) r.location = {loc->file, loc->line, loc->column};
        switch (i.op) {
        case Op::Constant: r.stack.push_back(i.literal); break;
        case Op::Load: {
            auto v = r.env.find(i.text); if (v == r.env.end()) throw std::runtime_error("unbound name: " + i.text);
            r.stack.push_back(v->second); break;
        }
        case Op::Closure: {
            Closure c; c.entry = i.target; c.parameter = i.text; c.self = i.extra;
            for (const auto& n : i.captures) c.captured.emplace(n, r.env.at(n));
            r.stack.push_back(function(std::move(c))); break;
        }
        case Op::Apply: case Op::TailApply: {
            auto arg = r.pop(), f = r.pop();
            if (f.kind() != Value::Kind::Function) throw std::runtime_error("attempt to call non-function");
            const auto& c = *f.data->closure;
            if (!c.builtin.empty()) {
                auto partial = c; partial.arguments.push_back(arg);
                if (partial.arguments.size() < c.arity) r.stack.push_back(function(std::move(partial)));
                else if (partial.arguments.size() == c.arity)
                    r.push_result(call_builtin(c.builtin, partial.arguments, r));
                else throw std::runtime_error("invalid builtin argument count");
            } else {
                if (i.op == Op::TailApply) {
                    if (r.frames.empty()) throw std::runtime_error("tail call outside function");
                    r.scopes.resize(r.frames.back().scopes); r.stack.resize(r.frames.back().stack);
                } else r.frames.push_back({vm.pc() + 1, r.scopes.size(), r.stack.size(), std::move(r.env)});
                r.env = c.captured;
                if (!c.self.empty()) r.env[c.self] = f;
                r.env[c.parameter] = arg;
                vm.set_pc(c.entry);
            }
            break;
        }
        case Op::Enter: r.scopes.push_back(r.env); break;
        case Op::Bind: r.env[i.text] = r.pop(); break;
        case Op::Leave:
            if (r.scopes.empty()) throw std::runtime_error("scope stack underflow");
            r.env = std::move(r.scopes.back()); r.scopes.pop_back(); break;
        case Op::Pop: r.pop(); break;
        case Op::Binary: {
            auto b = r.pop(), a = r.pop();
            r.push_result(r.binary_result(i.text, a, b)); break;
        }
        case Op::Unary: r.stack.push_back(unary(i.text, r.pop())); break;
        case Op::Pair: case Op::Cons: {
            auto b = r.pop(), a = r.pop();
            r.stack.push_back(value(i.op == Op::Pair ? Value::Kind::Pair : Value::Kind::Cons, 0, {}, {a, b})); break;
        }
        case Op::Jump: vm.set_pc(i.target); break;
        case Op::JumpFalse: if (!r.pop().boolean()) vm.set_pc(i.target); break;
        case Op::TryMatch: {
            if (r.stack.empty()) throw std::runtime_error("match stack underflow");
            Env bindings;
            if (match(i.pattern, r.stack.back(), bindings)) {
                r.pop(); r.scopes.push_back(r.env);
                for (const auto& [name, v] : bindings) r.env[name] = v;
            } else vm.set_pc(i.target);
            break;
        }
        case Op::Fail: throw std::runtime_error(i.text);
        case Op::Return: {
            if (r.frames.empty()) throw std::runtime_error("call stack underflow");
            auto result = r.pop(); auto frame = std::move(r.frames.back()); r.frames.pop_back();
            r.env = std::move(frame.env); r.scopes.resize(frame.scopes); r.stack.resize(frame.stack);
            r.stack.push_back(result); vm.set_pc(frame.pc); break;
        }
        case Op::Halt: return false;
        }
        return true;
    }
};
}
Execution run(const Program& program, Options options) {
    if (!program.impl) throw Error({}, "empty program handle");
    Runtime runtime; runtime.options = options; runtime.env = builtins();
    metavm::VMOptions config; config.max_steps = options.max_steps;
    metavm::VM<Instruction, ISA> vm(config); vm.load(program.impl->code);
    vm.reg(0) = metavm::Value::pointer(&runtime);
    auto status = vm.run();
    if (status != metavm::Status::Halted) {
        if (vm.diagnostic()) {
            const auto& d = *vm.diagnostic();
            throw Error({d.source.file, d.source.line, d.source.column}, d.message);
        }
        throw Error(runtime.location, "execution step limit exceeded");
    }
    if (runtime.stack.size() != 1) throw Error(runtime.location, "invalid final operand stack");
    return {runtime.pop(), std::move(runtime.output), vm.steps()};
}
std::string disassemble(const Program& program) {
    if (!program.impl) throw Error({}, "empty program handle");
    static const char* names[] = {"constant", "load", "closure", "apply", "tail_apply", "enter", "bind", "leave", "pop",
        "binary", "unary", "pair", "cons", "jump", "jump_false", "try_match", "fail", "return", "halt"};
    std::ostringstream out;
    for (std::size_t n = 0; n < program.impl->code.size(); ++n) {
        const auto& i = program.impl->code.code[n]; out << n << "  " << names[static_cast<unsigned>(i.op)];
        if (i.op == Op::Constant) out << ' ' << i.literal.show();
        if (!i.text.empty()) out << ' ' << i.text;
        if (i.op == Op::Closure || i.op == Op::Jump || i.op == Op::JumpFalse || i.op == Op::TryMatch) out << ' ' << i.target;
        out << '\n';
    }
    return out.str();
}
} // namespace gateml
