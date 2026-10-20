#include "../internal.hpp"
#include <libvm/BytecodeBinary.hpp>
#include <cmath>

namespace flintstone {
Registry::Registry() {
    detail::install_builtins(*this);
    detail::install_vector_dialect(*this);
    detail::install_tuple_dialect(*this);
    detail::install_ptr_dialect(*this);
}
std::vector<std::string> Registry::dialect_names() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : dialects_) names.push_back(name);
    return names;
}
std::vector<std::string> Registry::backend_names() const {
    std::vector<std::string> names;
    for (const auto& [name, _] : backends_) names.push_back(name);
    return names;
}
void Registry::add_dialect(DialectDefinition dialect) {
    using detail::check;
    check(detail::valid_name(dialect.name) && dialect.name.find('.') == std::string::npos && dialect.version,
          "dialect.name", "invalid dialect identity");
    check(!dialects_.count(dialect.name), "dialect.duplicate", "duplicate dialect " + dialect.name);
    std::set<std::string> names;
    for (const auto& [name, value] : dialect.parameters) {
        (void)value; check(detail::valid_name(name), "dialect.parameter", "invalid parameter name");
    }
    for (const auto& op : dialect.operations) {
        check(detail::valid_name(op.name) && detail::dialect_name(op.name) == dialect.name && names.insert(op.name).second,
              "dialect.operation", "invalid or duplicate operation " + op.name);
        check(op.min_operands <= op.max_operands && op.min_results <= op.max_results,
              "dialect.arity", "invalid operation arity interval");
        check(static_cast<unsigned>(op.level) <= static_cast<unsigned>(Level::Machine), "level", "invalid operation level");
        check(!op.phi || !op.terminator, "dialect.operation", "phi cannot be a terminator");
    }
    // Validation precedes insertion: malformed registrations leave the registry intact.
    for (const auto& op : dialect.operations) operations_.emplace(op.name, op);
    dialects_.emplace(dialect.name, std::move(dialect));
}
void Registry::add_lowering(LoweringRule rule) {
    detail::check(!rule.name.empty() && rule.rewrite, "lowering.rule", "invalid lowering rule");
    const auto& op = operation(rule.operation);
    detail::check(static_cast<unsigned>(rule.destination) > static_cast<unsigned>(op.level), "lowering.direction", "lowering must advance the IL level");
    for (const auto& old : lowerings_) detail::check(old.name != rule.name, "lowering.duplicate", "duplicate lowering rule name");
    lowerings_.push_back(std::move(rule));
}
void Registry::add_backend(std::string name, Backend backend) {
    detail::check(!name.empty() && backend && !backends_.count(name), "backend.registration", "invalid or duplicate backend");
    backends_.emplace(std::move(name), std::move(backend));
}
const OperationDefinition& Registry::operation(const std::string& name) const {
    auto found = operations_.find(name);
    detail::check(found != operations_.end(), "operation.unknown", "unregistered operation " + name);
    return found->second;
}
const DialectDefinition& Registry::dialect(const std::string& name) const {
    auto found = dialects_.find(name);
    detail::check(found != dialects_.end(), "dialect.unknown", "unregistered dialect " + name);
    return found->second;
}
Attributes Registry::parameters(const Module& module, const std::string& name) const {
    const auto& def = dialect(name);
    auto result = def.parameters;
    if (auto found = module.dialects.find(name); found != module.dialects.end()) for (const auto& [key, value] : found->second) {
        auto old = result.find(key);
        detail::check(old != result.end() && old->second.index() == value.index(), "dialect.parameter", "unknown or mistyped parameter " + name + '.' + key);
        old->second = value;
    }
    for (auto& [key, value] : result) if (auto type = std::get_if<Type>(&value)) *type = detail::resolve(*type, module);
    if (def.verify_parameters) def.verify_parameters(result, module.target);
    return result;
}
const Backend& Registry::backend(const std::string& name) const {
    auto found = backends_.find(name);
    detail::check(found != backends_.end(), "backend.unavailable", "backend is not registered: " + name);
    return found->second;
}

namespace detail {
namespace {
using Op = firestone::ir::Opcode;
void no_attributes(const Operation& op, const OperationContext&) { check(op.attributes.empty(), "attribute.unknown", "operation does not accept attributes", op.location); }
OperationDefinition scalar(std::string name, Op code, std::size_t inputs, std::size_t outputs, Level level = Level::Core) {
    OperationDefinition def;
    def.name = std::move(name); def.level = level;
    def.min_operands = def.max_operands = inputs; def.min_results = def.max_results = outputs;
    def.effect = Effect::Pure; def.ssa_opcode = code;
    if (code == Op::Load) def.effect = Effect::Read;
    if (code == Op::Store) def.effect = Effect::Write;
    if (code == Op::Call) { def.effect = Effect::Unknown; def.max_operands = SIZE_MAX; def.min_results = 0; }
    if (code == Op::Div || code == Op::Mod || code == Op::Shl || code == Op::Shr) def.effect = Effect::Unknown;
    if (code == Op::Phi) { def.phi = true; def.min_operands = 1; def.max_operands = SIZE_MAX; }
    if (firestone::ir::is_terminator(code)) { def.terminator = true; def.effect = Effect::Control; }
    if (code == Op::Return) { def.min_operands = 0; def.max_operands = 1; }
    def.verify = [code](const Operation& op, const OperationContext& context) {
        if (code != Op::Jump && code != Op::Branch) check(op.successors.empty(), "cf.targets", "operation cannot have successors", op.location);
        if (code == Op::Call) {
            check(op.attributes.size() == 1 && op.attributes.count("callee") && std::holds_alternative<std::string>(op.attributes.at("callee")),
                  "call.callee", "call requires exactly one string callee attribute", op.location);
            const auto& callee = std::get<std::string>(op.attributes.at("callee"));
            if (callee.rfind("__flintstone_", 0) == 0) {
                // Reserved runtime intrinsics (e.g. __flintstone_trap from the
                // cf.trap lowering) are validated by the executing backend,
                // which checks intrinsic names and signatures explicitly.
                // User modules cannot declare this prefix (symbol.reserved).
                return;
            }
            const auto& f = context.module.function(callee);
            check(f.parameters.size() == op.operands.size(), "call.arity", "call argument count differs from signature", op.location);
            for (std::size_t n = 0; n < op.operands.size(); ++n)
                check(context.type(op.operands[n]) == f.parameters[n].type, "call.type", "call argument type differs from signature", op.location);
            check((f.result.kind == TypeKind::Void && op.results.empty()) ||
                  (op.results.size() == 1 && op.results[0].type == f.result), "call.result", "call result differs from signature", op.location);
            return;
        }
        no_attributes(op, context);
        if (code == Op::Return) {
            check((context.function.result.kind == TypeKind::Void && op.operands.empty()) ||
                  (op.operands.size() == 1 && context.type(op.operands[0]) == context.function.result),
                  "return.type", "return does not match the function signature", op.location);
            return;
        }
        if (code == Op::Jump || code == Op::Branch) {
            check(op.successors.size() == (code == Op::Jump ? 1u : 2u), "cf.targets", "wrong number of control-flow successors", op.location);
            if (code == Op::Branch) check(context.type(op.operands[0]) == Type::integer(1), "cf.condition", "branch requires i1", op.location);
            return;
        }
        if (code == Op::Load || code == Op::Store) {
            const auto address = context.type(op.operands[0]);
            check(address.kind == TypeKind::Pointer, "memory.address", "memory address must have pointer type", op.location);
            // The portable VM models checked heap cells, not byte-addressed native
            // memory; other address spaces need an explicit target adapter.
            check(address.name == "generic", "memory.address-space", "VM memory requires ptr<generic>", op.location);
            return;
        }
        auto result = op.results.at(0).type;
        if (code == Op::Constant || code == Op::Copy || code == Op::Phi) {
            for (const auto& input : op.operands) check(context.type(input) == result, "type.mismatch", "operand type differs from result", op.location);
            if (code == Op::Constant) check(op.operands[0].kind == OperandKind::Integer || op.operands[0].kind == OperandKind::Float,
                                          "constant.literal", "constant requires a numeric literal", op.location);
            return;
        }
        if (code == Op::Select) {
            check(context.type(op.operands[0]) == Type::integer(1) && context.type(op.operands[1]) == result && context.type(op.operands[2]) == result,
                  "select.type", "select requires i1 and two values matching the result", op.location); return;
        }
        bool comparison = firestone::ir::is_comparison(code);
        auto input = context.type(op.operands[0]);
        check(input.kind == TypeKind::Integer || input.kind == TypeKind::Float, "arithmetic.type", "scalar arithmetic requires integer/float types", op.location);
        for (const auto& operand : op.operands) check(context.type(operand) == input, "type.mismatch", "arithmetic operand types differ", op.location);
        check(result == (comparison ? Type::integer(1) : input), "type.mismatch", "incorrect arithmetic result type", op.location);
        if ((code >= Op::Mod && code <= Op::Shr) || code == Op::Not)
            check(input.kind == TypeKind::Integer, "arithmetic.integer", "operation requires integers", op.location);
        if (input == Type::integer(1))
            check(comparison || code == Op::And || code == Op::Or || code == Op::Xor || code == Op::Not,
                  "arithmetic.boolean", "unsupported boolean arithmetic", op.location);
    };
    return def;
}
}
void install_builtins(Registry& registry) {
    DialectDefinition arith; arith.name = "arith";
    arith.parameters = {{"overflow", std::string("modular")}};
    arith.verify_parameters = [](const Attributes& attrs, const Target&) {
        check(std::get<std::string>(attrs.at("overflow")) == "modular", "arith.semantics", "only explicit modular integer semantics are supported");
    };
    arith.operations.push_back(scalar("arith.constant", Op::Constant, 1, 1));
    arith.operations.push_back(scalar("arith.copy", Op::Copy, 1, 1));
    const std::pair<const char*, Op> binaries[] = {{"add", Op::Add}, {"sub", Op::Sub}, {"mul", Op::Mul}, {"div", Op::Div},
        {"mod", Op::Mod}, {"and", Op::And}, {"or", Op::Or}, {"xor", Op::Xor}, {"shl", Op::Shl}, {"shr", Op::Shr},
        {"eq", Op::Eq}, {"ne", Op::Ne}, {"lt", Op::Lt}, {"le", Op::Le}, {"gt", Op::Gt}, {"ge", Op::Ge}};
    for (auto [name, op] : binaries) arith.operations.push_back(scalar(std::string("arith.") + name, op, 2, 1));
    arith.operations.push_back(scalar("arith.neg", Op::Neg, 1, 1));
    arith.operations.push_back(scalar("arith.not", Op::Not, 1, 1));
    arith.operations.push_back(scalar("arith.select", Op::Select, 3, 1));
    DialectDefinition cf; cf.name = "cf";
    cf.operations = {scalar("cf.phi", Op::Phi, 1, 1), scalar("cf.jump", Op::Jump, 0, 0), scalar("cf.branch", Op::Branch, 1, 0)};
    OperationDefinition trap;
    trap.name = "cf.trap";
    trap.level = Level::High;
    trap.min_operands = trap.max_operands = 0;
    trap.min_results = trap.max_results = 0;
    trap.effect = Effect::Control;
    trap.terminator = true;
    // No ssa_opcode: the trap lowering below replaces it with a throwing
    // runtime call plus a return, so scalar lowering never sees it.
    trap.verify = [](const Operation& op, const OperationContext&) {
        check(op.attributes.empty(), "attribute.unknown", "cf.trap accepts no attributes", op.location);
        check(op.successors.empty(), "cf.targets", "cf.trap takes no successors", op.location);
    };
    cf.operations.push_back(std::move(trap));
    DialectDefinition func; func.name = "func";
    func.operations = {scalar("func.call", Op::Call, 0, 1), scalar("func.return", Op::Return, 0, 0)};
    DialectDefinition mem; mem.name = "mem";
    mem.operations = {scalar("mem.load", Op::Load, 1, 1), scalar("mem.store", Op::Store, 2, 0)};
    // A real, portable virtual-machine dialect, not native-machine instructions.
    DialectDefinition vm; vm.name = "vm";
    for (const auto* dialect : {&arith, &cf, &func, &mem}) for (auto op : dialect->operations) {
        op.name = "vm." + op.name.substr(op.name.find('.') + 1); op.level = Level::Machine;
        vm.operations.push_back(std::move(op));
    }
    registry.add_dialect(arith); registry.add_dialect(cf); registry.add_dialect(func); registry.add_dialect(mem); registry.add_dialect(vm);
    DialectDefinition math; math.name = "math"; math.parameters = {{"factor", std::int64_t(1)}};
    std::vector<LoweringRule> math_rules;
    for (auto [name, count] :
         {std::pair{"square", 1u}, {"madd", 3u}, {"scale", 1u}, {"cube", 1u}, {"abs", 1u}}) {
        OperationDefinition op;
        op.name = std::string("math.") + name; op.min_operands = op.max_operands = count;
        op.min_results = op.max_results = 1; op.effect = Effect::Pure;
        op.verify = [](const Operation& operation, const OperationContext& context) {
            no_attributes(operation, context);
            const auto& type = operation.results.at(0).type;
            check(type.kind == TypeKind::Integer && type.bits >= 2 && type.bits <= 64, "math.type", "math operations require i2..i64", operation.location);
            for (const auto& input : operation.operands) check(context.type(input) == type, "math.type", "math operand/result types must agree", operation.location);
            if (operation.name == "math.scale") {
                auto factor = std::get<std::int64_t>(context.dialect_parameters.at("factor"));
                if (type.bits < 64) check(factor >= -(std::int64_t(1) << (type.bits - 1)) && factor < (std::int64_t(1) << (type.bits - 1)),
                                          "math.factor", "dialect factor does not fit the result type", operation.location);
            }
        };
        math.operations.push_back(op);
        math_rules.push_back({std::string("math-") + name, op.name, Level::Core,
            [name = std::string(name)](const Operation& source, const Function& f, const Module& m, const Registry& r) {
                const auto& type = source.results[0].type;
                auto fresh = f.fresh_value();
                auto temp = [&] {
                    check(fresh < invalid_id - 1, "id.exhausted", "lowering exhausted value IDs", source.location);
                    return fresh++;
                };
                if (name == "cube") {
                    Operation square = source; square.name = "arith.mul";
                    const auto t = temp();
                    square.results = {{t, type, source.location}};
                    square.operands = {source.operands[0], source.operands[0]};
                    Operation product = source; product.name = "arith.mul";
                    product.operands = {Operand::ref(t), source.operands[0]};
                    return std::vector<Operation>{std::move(square), std::move(product)};
                }
                if (name == "abs") {
                    // abs(x) = select(x < 0, -x, x); neg(MIN) wraps to MIN under modular semantics.
                    Operation compare = source; compare.name = "arith.lt";
                    const auto c = temp();
                    compare.results = {{c, Type::integer(1), source.location}};
                    compare.operands.push_back(Operand::constant(0, type));
                    Operation negate = source; negate.name = "arith.neg";
                    const auto n = temp();
                    negate.results = {{n, type, source.location}};
                    negate.operands.resize(1);
                    Operation select = source; select.name = "arith.select";
                    select.operands = {Operand::ref(c), Operand::ref(n), source.operands[0]};
                    return std::vector<Operation>{std::move(compare), std::move(negate), std::move(select)};
                }
                Operation product = source; product.name = "arith.mul";
                if (name == "square") product.operands = {source.operands[0], source.operands[0]};
                if (name == "scale") product.operands.push_back(Operand::constant(std::get<std::int64_t>(r.parameters(m, "math").at("factor")), source.results[0].type));
                if (name != "madd") return std::vector<Operation>{std::move(product)};
                auto temporary = temp();
                product.results = {{temporary, source.results[0].type, source.location}};
                product.operands.resize(2);
                Operation sum = source; sum.name = "arith.add"; sum.operands = {Operand::ref(temporary), source.operands[2]};
                return std::vector<Operation>{std::move(product), std::move(sum)};
            }});
    }
    registry.add_dialect(std::move(math));
    for (auto& rule : math_rules) registry.add_lowering(std::move(rule));
    // Registered before the vm renames below so machine lowering reaches Core
    // first (the Core call/return then rename to vm.* normally).
    registry.add_lowering({"cf-trap", "cf.trap", Level::Core,
        [](const Operation& source, const Function& f, const Module&, const Registry&) {
            Operation call;
            call.name = "func.call";
            call.location = source.location;
            call.attributes["callee"] = std::string("__flintstone_trap");
            Operation ret;
            ret.name = "func.return";
            ret.location = source.location;
            if (f.result.kind == TypeKind::Void) return std::vector<Operation>{std::move(call), std::move(ret)};
            const auto result = f.fresh_value();
            call.results = {{result, f.result, source.location}};
            ret.operands = {Operand::ref(result)};
            return std::vector<Operation>{std::move(call), std::move(ret)};
        }});
    // cf.trap already lowers to Core above; a machine rename to a nonexistent
    // vm.trap operation must never be created.
    for (const auto* dialect : {&arith, &cf, &func, &mem}) for (const auto& op : dialect->operations) {
        if (op.name == "cf.trap") continue;
        registry.add_lowering({"vm-" + op.name, op.name, Level::Machine,
            [](const Operation& source, const Function&, const Module& module, const Registry&) {
                check(module.target.name == "vm", "target.lowering", "builtin machine lowering requires target vm", source.location);
                Operation result = source; result.name = "vm." + source.name.substr(source.name.find('.') + 1);
                return std::vector<Operation>{std::move(result)};
            }});
    }
    registry.add_backend("vm", [](const Module& module, const Registry& registry) {
        return Artifact{"fsvm", firestone::vm::encode(compile_vm(module, registry)), {}, {}};
    });
    registry.add_backend("vm-stack", [](const Module& module, const Registry& registry) {
        return Artifact{"fsvm", firestone::vm::encode(compile_vm(module, registry, firestone::vm::VMModel::Stack)), {}, {}};
    });
}
}
} // namespace flintstone
