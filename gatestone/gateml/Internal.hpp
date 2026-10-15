#pragma once
#include "GateML.h"
#include "MetaChain.hpp"
#include <map>
#include <set>
#include <variant>

namespace gateml {
struct Pattern {
    enum Kind { Wild, Bind, Integer, Boolean, Unit, Nil, Cons, Pair, None, Some, Ok, Error } kind = Wild;
    std::string text;
    std::int64_t number = 0;
    Location loc;
    std::vector<Pattern> children;
};
struct Expr;
using Tree = std::shared_ptr<Expr>;
struct Expr {
    enum Kind { Integer, Boolean, String, Unit, Variable, Lambda, Apply, Let, If,
                Binary, Unary, Pair, Nil, Cons, Sequence, Match } kind = Unit;
    Location loc;
    std::string text;
    std::int64_t number = 0;
    bool recursive = false;
    std::size_t height = 1;
    std::vector<Tree> children;
    std::vector<Pattern> patterns;
};
Tree parse(std::string_view source, const std::string& file, const Options& options);
std::string preprocess(std::string_view source, const std::string& file);

struct Type;
using Ty = std::shared_ptr<Type>;
struct Type {
    std::string name;
    unsigned id = 0;
    Ty link;
    std::vector<Ty> args;
};
struct Scheme { std::set<unsigned> variables; Ty type; };
using TypeEnv = std::map<std::string, Scheme>;
Ty type(std::string name, std::vector<Ty> args = {});
Ty prune(Ty t);
std::string type_name(Ty t);
Ty infer(const Tree& tree, TypeEnv env, unsigned& next, const Options& options);
TypeEnv builtin_types(unsigned& next);

struct Closure;
using Env = std::map<std::string, Value>;
struct Value::Data {
    Kind kind = Kind::Unit;
    std::int64_t number = 0;
    std::string text;
    std::vector<Value> fields;
    std::shared_ptr<const Closure> closure;
};
struct Closure {
    std::size_t entry = 0;
    std::string parameter, self, builtin;
    Env captured;
    std::vector<Value> arguments;
    std::size_t arity = 0;
};
Value value(Value::Kind kind, std::int64_t number = 0, std::string text = {}, std::vector<Value> fields = {});
Value function(Closure closure);
bool equal(const Value& a, const Value& b, std::size_t depth = 0);
bool match(const Pattern& p, const Value& v, Env& bindings);

enum class Op { Constant, Load, Closure, Apply, TailApply, Enter, Bind, Leave, Pop,
                Binary, Unary, Pair, Cons, Jump, JumpFalse, TryMatch, Fail, Return, Halt };
struct Instruction {
    Op op = Op::Halt;
    std::size_t target = 0;
    std::string text, extra;
    Value literal;
    Pattern pattern;
    std::set<std::string> captures;
};
struct Program::Impl {
    metavm::Program<Instruction> code;
    Tree tree;
};
std::shared_ptr<Program::Impl> lower(Tree tree, const Options& options);
void optimize(Tree& tree);
struct Runtime {
    Options options;
    Env env;
    std::vector<Env> scopes;
    std::vector<Value> stack;
    struct Frame { std::size_t pc, scopes, stack; Env env; };
    std::vector<Frame> frames;
    std::string output;
    Location location;
    std::size_t allocations = 0;
    std::size_t string_bytes = 0;
    void account();
    void write(std::string_view text);
    void push_result(Value result);
    Value binary_result(const std::string& op, const Value& a, const Value& b) const;
    Value pop();
};
Env builtins();
Value call_builtin(const std::string& name, const std::vector<Value>& args, Runtime& runtime);
Value binary(const std::string& op, const Value& a, const Value& b);
Value unary(const std::string& op, const Value& v);
} // namespace gateml
