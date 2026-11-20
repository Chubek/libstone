#include <cephyr/lower.hpp>
#include <cephyr/preprocess.hpp>

#include "Backend.h"

#include <AbstractSyntaxTree.h>
#include <wetstone/utils/Graph.hpp>

#include <algorithm>
#include <charconv>
#include <locale>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>

namespace cephyr::detail {
namespace {

constexpr std::size_t max_blocks = 256, max_values = 4096;
constexpr std::size_t max_symbols = 256;
constexpr std::size_t max_function_instructions = 16384, max_module_instructions = 65536;
constexpr std::size_t max_mir = 4u * 1024u * 1024u;
constexpr std::string_view reserved_prefix = "__cephyr_tu_";

std::string number(Id id)
{
    return id == no_value ? "-" : std::to_string(id);
}

bool symbol_name(std::string_view name)
{
    const auto head = [](unsigned char c) {
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    return !name.empty() && name.size() <= 255 && head(name.front()) &&
        std::all_of(name.begin() + 1, name.end(), [&](unsigned char c) {
            return head(c) || (c >= '0' && c <= '9');
        });
}

bool backend_signature(const CType &type)
{
    if (type.kind != CType::Kind::function || !type.element ||
        !(type.element->kind == CType::Kind::void_ || type.element->scalar()) ||
        type.variadic || type.params.size() > ARCHON_MIR_MAX_PARAMS)
        return false;
    return std::all_of(type.params.begin(), type.params.end(), [](const CType &t) { return t.scalar(); });
}

CType declaration_type(cparser::FunctionDeclASTNode &node, Unit &unit)
{
    auto *result = node.getType();
    if (present(result) && result->getKind() == cparser::NK_FUNCTION_TYPE)
        return read_type(result, TypeContext(unit), true);
    cparser::FunctionTypeASTNode function(result, node.getPrms());
    return read_type(&function, TypeContext(unit), true);
}

bool valid_main(const CType &type, const Preprocessed &source)
{
    if (!type.element || !type.element->integer() || type.element->bits != 32 ||
        !type.element->is_signed || type.variadic)
        return false;
    if (type.params.empty())
        return true;
    if (type.params.size() != 2)
        return false;
    const auto &argc = type.params[0];
    const auto &argv = type.params[1];
    return compatible(argc, integer_type()) &&
        compatible(argv, pointer_type(pointer_type(plain_char_type(source))));
}

// A declaration is made visible before lowering its initializer or body; a
// later name must not accidentally satisfy an earlier C use.
Symbol *declare(Unit &unit, Node *declaration, Node *name_node, CType type,
                unsigned flags, bool is_definition, bool has_initializer)
{
    const std::string name = identifier(name_node);
    const bool function = type.kind == CType::Kind::function;
    const bool static_spec = flags & SCS_STATIC;
    const bool extern_spec = flags & SCS_EXTERN;
    if (!symbol_name(name) || name.starts_with(reserved_prefix)) {
        unit.error(name_node, "unsupported or reserved C identifier for native output");
        return nullptr;
    }
    if ((static_spec && extern_spec) || (flags & (SCS_AUTO | SCS_REGISTER))) {
        unit.error(name_node, "invalid file-scope storage class");
        return nullptr;
    }
    if (!type.valid())
        return nullptr;
    auto found = unit.symbols.find(name);
    const bool inherits = !static_spec && (extern_spec || function);
    const bool internal = static_spec || (inherits && found != unit.symbols.end() && found->second.internal);
    if (found == unit.symbols.end()) {
        std::string native_name = internal ? std::string(reserved_prefix) + "local_" + name : name;
        if (!symbol_name(native_name)) {
            unit.error(name_node, "identifier exceeds the native backend's symbol limit");
            return nullptr;
        }
        Symbol symbol{
            std::move(type), std::move(native_name), declaration,
            is_definition ? declaration : nullptr, internal, false, has_initializer,
        };
        return &unit.symbols.emplace(name, std::move(symbol)).first->second;
    }
    auto &symbol = found->second;
    if (!compatible(symbol.type, type) ||
        (function && symbol.type.prototype != type.prototype &&
         ((symbol.definition && !symbol.type.prototype && !type.params.empty()) ||
          (is_definition && !type.prototype && !symbol.type.params.empty())))) {
        unit.error(name_node, "conflicting types for '" + name + "'");
        return nullptr;
    }
    if (internal != symbol.internal) {
        unit.error(name_node, "conflicting linkage for '" + name + "'");
        return nullptr;
    }
    if (symbol.definition && is_definition && (function || (symbol.initialized && has_initializer))) {
        unit.error(name_node, "redefinition of '" + name + "'");
        return nullptr;
    }
    if (function ? (!symbol.type.prototype || is_definition) :
        (type.kind == CType::Kind::array && !symbol.type.count && type.count))
        symbol.type = std::move(type);
    if (is_definition && (!symbol.definition || has_initializer)) {
        symbol.definition = declaration;
        symbol.initialized = has_initializer;
    }
    return &symbol;
}

void write_edge(std::ostream &out, const Edge &edge, const std::vector<Id> &blocks,
                const std::vector<Id> &values)
{
    out << ' ' << number(blocks.at(edge.target)) << ' ' << edge.args.size();
    for (Id arg : edge.args)
        out << ' ' << number(values.at(arg));
}

Id read_id(std::string_view text)
{
    Id result = no_value;
    auto status = std::from_chars(text.data(), text.data() + text.size(), result);
    if (status.ec != std::errc{} || status.ptr != text.data() + text.size())
        throw std::logic_error("invalid native operand from the C frontend");
    return result;
}

// Record SSA definitions independently of their formatting. After Wetstone
// pruning, remap SSA operands only (never immediates, counts or symbols).
void write_instruction(std::ostream &out, const Instruction &inst, const std::vector<Id> &values)
{
    out << inst.opcode;
    if (inst.result != no_value)
        out << ' ' << number(values.at(inst.result));

    const std::string_view op = inst.opcode;
    const bool call = op == "call" || op == "call_indirect";
    const bool indirect = op == "call_indirect";
    const std::size_t void_dst = call && inst.result == no_value ? 1 : 0;
    const unsigned fixed =
        op == "const" || op == "frame_addr" || op == "global_addr" || op == "func_addr" ? 0 :
        op == "copy" || op == "sext" || op == "zext" || op == "trunc" ||
        op.ends_with("_to_int") || op.starts_with("int_to_") || op.starts_with("load") ? 1 : 2;

    std::string_view operands = inst.operands;
    for (std::size_t i = 0; !operands.empty();) {
        auto start = operands.find_first_not_of(' ');
        if (start == std::string_view::npos)
            break;
        operands.remove_prefix(start);
        const auto end = operands.find(' ');
        const auto token = operands.substr(0, end);
        const bool id = call ? ((indirect && i == void_dst) || i >= void_dst + (indirect ? 3 : 2))
                             : i < fixed;
        if (id)
            out << ' ' << number(values.at(read_id(token)));
        else
            out << ' ' << token;
        ++i;
        if (end == std::string_view::npos)
            break;
        operands.remove_prefix(end);
    }
    out << '\n';
}

std::string write_function(const Function &f, std::size_t &module_instructions)
{
    const auto reachable = reachable_blocks(f.blocks);
    std::vector<Id> block_ids(f.blocks.size(), no_value), value_ids(f.values.size(), no_value);
    Id block_count = 0, value_count = 0;
    const auto params = f.symbol.type.params.size();
    for (std::size_t i = 0; i < params; ++i)
        value_ids.at(i) = value_count++;
    for (const auto &inst : f.entry_addresses)
        value_ids.at(inst.result) = value_count++;
    std::size_t instructions = f.entry_addresses.size();
    for (std::size_t i = 0; i < f.blocks.size(); ++i) {
        if (!reachable[i])
            continue;
        block_ids[i] = block_count++;
        for (Id id : f.blocks[i].params)
            value_ids.at(id) = value_count++;
        for (const auto &inst : f.blocks[i].instructions) {
            ++instructions;
            if (inst.result != no_value)
                value_ids.at(inst.result) = value_count++;
        }
    }
    if (instructions > max_function_instructions ||
        instructions > max_module_instructions - module_instructions)
        throw std::length_error("native backend instruction limit exceeded");
    module_instructions += instructions;

    std::vector<std::string_view> types(value_count);
    for (std::size_t i = 0; i < f.values.size(); ++i)
        if (value_ids[i] != no_value)
            types[value_ids[i]] = mir_type(f.values[i]);

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "function " << f.symbol.name << ' ' << mir_type(*f.symbol.type.element) << ' '
        << params << ' ' << value_count << ' ' << block_count << ' ' << f.locals.size() << "\ntypes";
    for (auto type : types)
        out << ' ' << type;
    out << '\n';
    for (const auto &[size, alignment] : f.locals)
        out << "local " << size << ' ' << alignment << '\n';
    for (std::size_t i = 0; i < f.blocks.size(); ++i) {
        if (!reachable[i])
            continue;
        const auto &block = f.blocks[i];
        out << "block " << block.params.size() << ' '
            << block.instructions.size() + (i == 0 ? f.entry_addresses.size() : 0) << "\nparams";
        for (Id id : block.params)
            out << ' ' << number(value_ids.at(id));
        out << '\n';
        if (i == 0)
            for (const auto &inst : f.entry_addresses)
                write_instruction(out, inst, value_ids);
        for (const auto &inst : block.instructions)
            write_instruction(out, inst, value_ids);
        const auto &term = block.term;
        switch (term.kind) {
        case Term::Kind::ret:
            out << "ret " << (term.value == no_value ? "-" : number(value_ids.at(term.value)));
            break;
        case Term::Kind::jump:
            out << "br";
            write_edge(out, term.yes, block_ids, value_ids);
            break;
        case Term::Kind::branch:
            out << "cbr " << number(value_ids.at(term.value));
            write_edge(out, term.yes, block_ids, value_ids);
            write_edge(out, term.no, block_ids, value_ids);
            break;
        case Term::Kind::trap:
            out << "trap";
            break;
        case Term::Kind::open:
            throw std::logic_error("unterminated reachable block from the C frontend");
        }
        out << '\n';
    }
    return out.str();
}

void write_import(std::ostream &out, const std::string &name, const CType &type)
{
    out << "import " << name << ' ' << mir_type(*type.element) << ' ' << type.params.size();
    for (const auto &param : type.params)
        out << ' ' << mir_type(param);
    out << '\n';
}

bool character_array(const CType &type)
{
    return type.kind == CType::Kind::array && type.element &&
        type.element->integer() && type.element->bits == 8;
}

class ArrayInitPlanner {
public:
    Unit &unit;
    ArrayInitialization plan;

    explicit ArrayInitPlanner(Unit &u) : unit(u) {}

    bool initialize(CType &type, Node *node, std::uint64_t offset)
    {
        if (type.kind != CType::Kind::array) {
            plan.entries.push_back({offset, node, std::nullopt});
            return true;
        }

        const bool braced = node->getKind() == cparser::NK_LIST;
        const auto children = braced ? elements(node) : std::vector<Node *>{};
        Node *first = braced && !children.empty() ? children.front() : node;
        if (character_array(type) && first->getKind() == cparser::NK_STRING_CONST) {
            if (children.size() > 1)
                return error(children[1], "too many elements after character-array string initializer");
            auto bytes = string_constant(first, unit.source, unit.diagnostics);
            if (!bytes)
                return false;
            if (!type.count)
                type.count = bytes->size();
            if (bytes->size() - 1 > type.count)
                return error(first, "string literal is too long for character array");
            if (bytes->size() > type.count)
                bytes->pop_back(); // C11 6.7.9p14: only omit the final terminator.
            plan.entries.push_back({offset, first, std::move(bytes)});
            return true;
        }
        if (!braced)
            return error(node, "array initializer must be a brace-enclosed list");
        if (children.empty())
            return error(node, "array initializer list must not be empty");

        std::size_t next = 0;
        auto count = consume(type, children, next, offset);
        if (!count)
            return false;
        if (next != children.size())
            return error(children[next], "too many elements in array initializer");
        if (!type.count)
            type.count = *count;
        return true;
    }

private:
    // The explicit braces delimit a cursor. With elided braces a nested
    // array consumes just enough entries from its parent's cursor (6.7.9p20).
    std::optional<std::uint64_t> consume(const CType &type, std::span<Node *const> list,
                                         std::size_t &next, std::uint64_t base)
    {
        const AnalysisDepth guard(depth);
        const auto &element = *type.element;
        const auto stride = size_of(element);
        if (!stride || !*stride) {
            error(list[next], "array initializer requires a complete element type");
            return std::nullopt;
        }
        std::uint64_t index = 0;
        for (; next != list.size() && (!type.count || index < type.count); ++index) {
            Node *node = list[next];
            // Check the entire subobject, not just the location of its first
            // explicitly initialized leaf, before adding offsets.
            if (index >= (std::numeric_limits<std::uint64_t>::max() - base) / *stride) {
                error(node, "array initializer exceeds the object size limit");
                return std::nullopt;
            }
            const std::uint64_t offset = base + index * *stride;
            if (element.kind == CType::Kind::array && node->getKind() != cparser::NK_LIST &&
                !(character_array(element) && node->getKind() == cparser::NK_STRING_CONST)) {
                if (!consume(element, list, next, offset))
                    return std::nullopt;
            } else {
                ++next;
                CType subobject = element;
                if (!initialize(subobject, node, offset))
                    return std::nullopt;
            }
        }
        return index;
    }

    bool error(Node *node, std::string_view message)
    {
        unit.error(node, message, "cephyr.array.initializer");
        return false;
    }

    unsigned depth = 0;
};

} // namespace

std::optional<ArrayInitialization> plan_array_initializer(Unit &unit, CType &type, Node *node)
{
    const CType *scalar = &type;
    bool volatile_array = false;
    while (scalar->kind == CType::Kind::array && scalar->element) {
        volatile_array |= scalar->is_volatile;
        scalar = scalar->element.get();
    }
    if (!scalar->scalar()) {
        unit.error(node, "array initialization requires a supported scalar element type",
                   "cephyr.array.initializer");
        return std::nullopt;
    }
    ArrayInitPlanner planner(unit);
    planner.plan.scalar = *scalar;
    planner.plan.scalar.is_volatile |= volatile_array;
    if (!planner.initialize(type, node, 0))
        return std::nullopt;
    return std::move(planner.plan);
}

void Unit::error(Node *at, std::string_view message, std::string_view code)
{
    diagnostics.error(code, message, location(source, at));
}

void Unit::warn(Node *at, std::string_view message, std::string_view code)
{
    diagnostics.warning(code, message, location(source, at));
}

TypeContext::TypeContext(Unit &u, const Function *f)
    : unit(u), source(u.source), diagnostics(u.diagnostics), function(f)
{}

TypeContext TypeContext::with_probe(Diagnostics &probe) const
{
    return {unit, source, probe, function, prototype, true};
}

TypeBinding TypeContext::lookup(std::string_view name) const
{
    const std::string key(name);
    for (auto scope = prototype; scope; scope = scope->parent)
        if (const auto found = scope->parameters.find(key); found != scope->parameters.end())
            return found->second;
    if (function)
        if (const auto local = function->find_local(key))
            return {local->type, local->register_object};
    const auto symbol = unit.symbols.find(key);
    return symbol == unit.symbols.end() ? TypeBinding{} : TypeBinding{symbol->second.type};
}

std::string Unit::require_signature(const CType &type)
{
    for (const auto &[name, signature] : signatures)
        if (name.starts_with("__cephyr_tu_signature_") && compatible(type, signature))
            return name;
    const auto name = std::string(reserved_prefix) + "signature_" + std::to_string(signatures.size());
    signatures.emplace_back(name, type);
    return name;
}

std::string Unit::require_variadic_call(const Symbol &symbol, const std::vector<CType> &args)
{
    // With only integer and pointer arguments, each LP64 variadic
    // argument occupies one XLEN slot after the C default promotions. The
    // separate native MIR import records this particular call's ABI; GAS
    // resolves its alias to the actual external function.
    CType signature = symbol.type;
    signature.params = args;
    signature.variadic = false;
    auto &related = variadic_signatures[symbol.name];
    for (auto index : related)
        if (compatible(signature, signatures[index].second))
            return signatures[index].first;
    const auto alias = std::string(reserved_prefix) + "vararg_" + std::to_string(signatures.size());
    related.push_back(signatures.size());
    signatures.emplace_back(alias, std::move(signature));
    data_assembly += "\t.set " + alias + ", " + symbol.name + "\n";
    return alias;
}

void check_static_assert(const TypeContext &context, Node *node)
{
    auto &unit = context.unit;
    auto &assertion = *static_cast<cparser::StaticAssertASTNode *>(node);
    const auto count = unit.diagnostics.error_count();
    const auto condition = integer_constant(assertion.getExpr(), context);
    const auto message = string_constant(assertion.getMessage(), unit.source, unit.diagnostics);
    if (!condition) {
        if (unit.diagnostics.error_count() == count)
            unit.error(node, "_Static_assert requires an integer constant expression", "cephyr.static_assert");
    } else if (message && normalize(condition->value, condition->type) == 0) {
        std::string description = "static assertion failed";
        std::string_view literal(*message);
        if (!literal.empty() && literal.back() == '\0')
            literal.remove_suffix(1);
        if (!literal.empty())
            description += ": " + std::string(literal);
        unit.error(node, description, "cephyr.static_assert");
    }
}

bool Function::open() const
{
    return current < blocks.size() && blocks[current].term.kind == Term::Kind::open;
}

Id Function::block()
{
    if (blocks.size() == max_blocks)
        throw std::length_error("native backend block limit exceeded");
    const auto id = static_cast<Id>(blocks.size());
    blocks.emplace_back();
    return id;
}

void Function::select(Id id)
{
    current = id;
}

Id Function::param(Id target, const CType &type)
{
    const Id id = value(type);
    blocks.at(target).params.push_back(id);
    return id;
}

void Function::jump(Id dest, std::vector<Id> args)
{
    if (open())
        blocks[current].term = {Term::Kind::jump, no_value, {dest, std::move(args)}, {}};
}

void Function::branch(Id cond, Id yes, Id no, std::vector<Id> yes_args, std::vector<Id> no_args)
{
    if (open())
        blocks[current].term = {Term::Kind::branch, cond, {yes, std::move(yes_args)}, {no, std::move(no_args)}};
}

void Function::ret(Id val)
{
    if (open())
        blocks[current].term = {Term::Kind::ret, val};
}

void Function::trap()
{
    if (open())
        blocks[current].term = {Term::Kind::trap};
}

Id Function::value(const CType &type)
{
    if (values.size() == max_values)
        throw std::length_error("native backend SSA value limit exceeded");
    const auto id = static_cast<Id>(values.size());
    values.push_back(type);
    return id;
}

Id Function::emit(const CType &type, std::string_view op, std::string operands)
{
    if (!open())
        throw std::logic_error("instruction emitted into a closed block");
    const auto id = value(type);
    blocks[current].instructions.push_back({std::string(op), std::move(operands), id});
    return id;
}

void Function::emit_void(std::string_view op, std::string operands)
{
    if (!open())
        throw std::logic_error("instruction emitted into a closed block");
    blocks[current].instructions.push_back({std::string(op), std::move(operands)});
}

Id Function::constant(std::uint64_t bits, const CType &type)
{
    return emit(type, "const", std::to_string(normalize(bits, type)));
}

LValue Function::local(const CType &type, Node *at)
{
    const auto size = size_of(type);
    const auto alignment = align_of(type);
    if (!size || !*size || *size > std::numeric_limits<int>::max() || !alignment || alignment > 16) {
        error(at, "local object has incomplete or unsupported size or alignment");
        return {};
    }
    const auto index = locals.size();
    locals.emplace_back(static_cast<unsigned>(*size), alignment);
    // The stack slot exists for the whole function regardless of when its
    // source declaration or initializer is reached.
    const auto address = value(pointer_type(type));
    entry_addresses.push_back({"frame_addr", std::to_string(index), address});
    return {type, address};
}

std::optional<LValue> Function::find_local(std::string_view name) const
{
    const std::string key(name);
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        if (const auto found = scope->find(key); found != scope->end())
            return found->second;
    return std::nullopt;
}

bool Function::declare_local(std::string name, LValue storage, Node *at)
{
    if (!symbol_name(name)) {
        error(at, "unsupported local identifier");
        return false;
    }
    if (!scopes.back().emplace(name, std::move(storage)).second) {
        error(at, "redeclaration of '" + name + "' in the same block");
        return false;
    }
    return true;
}

void Function::require_static(const LValue &local) const
{
    if (!local.static_name.empty() && local.static_name == function_name_symbol)
        unit.require_function_name(function_name_symbol, source_name);
}

void Function::error(Node *at, std::string_view message, std::string_view code)
{
    unit.error(at, message, code);
}

void Function::warn(Node *at, std::string_view message, std::string_view code)
{
    unit.warn(at, message, code);
}

std::vector<bool> reachable_blocks(const std::vector<Block> &blocks)
{
    namespace ir = wetstone::ir;
    ir::Function graph;
    graph.entry = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const auto &term = blocks[i].term;
        ir::Block block;
        block.id = static_cast<ir::BlockId>(i);
        ir::Instruction terminator;
        if (term.kind == Term::Kind::jump) {
            terminator.opcode = ir::Opcode::Jump;
            terminator.targets = {term.yes.target};
        } else if (term.kind == Term::Kind::branch) {
            terminator.opcode = ir::Opcode::Branch;
            terminator.targets = {term.yes.target, term.no.target};
            if (term.value != no_value)
                terminator.arguments = {ir::Operand::ref(term.value)};
            else
                terminator.arguments = {ir::Operand::constant(0)}; // failed source expression
        } else {
            terminator.opcode = ir::Opcode::Return; // trap and open have no successors
        }
        block.instructions.push_back(std::move(terminator));
        graph.blocks.push_back(std::move(block));
    }
    graph.validate();
    std::vector<bool> result(blocks.size());
    for (auto id : wetstone::graph::reachable(graph))
        result[id] = true;
    return result;
}

} // namespace cephyr::detail

namespace cephyr {

std::optional<LoweredUnit> lower(cparser::ASTNode *root, const Preprocessed &source,
                                 Diagnostics &diagnostics, bool hosted)
{
    using namespace detail;
    using namespace cparser;

    Unit unit(source, diagnostics, hosted);
    std::vector<std::string> functions;
    std::set<std::string> typedef_names;
    std::size_t instruction_count = 0;
    const auto before = diagnostics.error_count();
    Node *at = root;
    try {
        for (Node *node : elements(root)) {
            at = node;
            if (!present(node))
                continue;
            if (node->getKind() == NK_STATIC_ASSERT) {
                check_static_assert(TypeContext(unit), node);
                continue;
            }
            if (node->getKind() == NK_TYPE_DECL) {
                auto &decl = *static_cast<TypeDeclASTNode *>(node);
                const auto name = identifier(decl.getName());
                if (!name.empty() && unit.symbols.contains(name))
                    unit.error(decl.getName(), "typedef conflicts with '" + name + "'");
                if (!name.empty())
                    typedef_names.insert(name);
                bind_typedef_bounds(decl.getBody(), TypeContext(unit));
                continue;
            }
            if (node->getKind() == NK_FUNCTION_DECL) {
                auto &decl = *static_cast<FunctionDeclASTNode *>(node);
                if (typedef_names.contains(identifier(decl.getName()))) {
                    unit.error(decl.getName(), "function name conflicts with a typedef");
                    continue;
                }
                const bool definition = present(decl.getBody());
                auto type = declaration_type(decl, unit);
                const auto flags = decl.getFlags() |
                    (present(decl.getType()) ? decl.getType()->getFlags() : 0);
                auto *symbol = declare(unit, node, decl.getName(), std::move(type), flags, definition, false);
                if (!symbol || !definition)
                    continue;
                if (!backend_signature(symbol->type)) {
                    unit.error(decl.getName(), "function signature is unsupported by the native backend");
                    continue;
                }
                if (hosted && identifier(decl.getName()) == "main" &&
                    (symbol->internal || !valid_main(symbol->type, unit.source))) {
                    unit.error(decl.getName(), "hosted 'main' must have external linkage and type int(void) or int(int, char **)");
                    continue;
                }
                const auto errors = diagnostics.error_count();
                Function f(unit, *symbol, identifier(decl.getName()));
                lower_function_body(f, decl.getPrms(), decl.getBody());
                if (errors == diagnostics.error_count()) {
                    functions.push_back(write_function(f, instruction_count));
                    if (symbol->internal)
                        unit.locals.push_back(symbol->name);
                }
                continue;
            }
            if (node->getKind() == NK_VAR_DECL) {
                auto &decl = *static_cast<VarDeclASTNode *>(node);
                if (typedef_names.contains(identifier(decl.getName()))) {
                    unit.error(decl.getName(), "object name conflicts with a typedef");
                    continue;
                }
                auto type = read_type(decl.getType(), TypeContext(unit), true);
                const auto flags = decl.getFlags() |
                    (present(decl.getType()) ? decl.getType()->getFlags() : 0);
                const bool initialized = present(decl.getInit());
                if (type.kind == CType::Kind::void_ ||
                    (type.kind == CType::Kind::function && initialized)) {
                    unit.error(decl.getName(), "invalid file-scope object type or initializer");
                    continue;
                }
                const bool definition = type.kind != CType::Kind::function &&
                    (initialized || !(flags & SCS_EXTERN));
                auto *symbol = declare(unit, node, decl.getName(), std::move(type), flags, definition, initialized);
                // Initializers see exactly the declarations visible here.
                // This also completes `char s[] = "...";` before any later
                // use of sizeof(s). Tentative definitions are finalized below.
                if (symbol && definition && initialized)
                    define_global(unit, *symbol);
                continue;
            }
            if (node->getKind() == NK_STRUCT_TYPE || node->getKind() == NK_UNION_TYPE ||
                node->getKind() == NK_ENUMERAL_TYPE)
                continue; // tag-only declaration; read_type handles uses
            unit.error(node, "unsupported file-scope C construct");
        }

        std::vector<std::pair<std::string, CType>> imports = unit.signatures;
        for (auto &[source_name, symbol] : unit.symbols) {
            at = symbol.declaration;
            if (symbol.type.kind == CType::Kind::function) {
                if (!symbol.definition && symbol.used) {
                    if (symbol.internal)
                        unit.error(at, "internal function '" + source_name + "' has no definition");
                    else if (backend_signature(symbol.type))
                        imports.emplace_back(symbol.name, symbol.type);
                    else
                        unit.error(at, "external function signature is unsupported by the native backend");
                }
            } else if (symbol.definition && !symbol.initialized) {
                at = symbol.definition;
                if (!symbol.internal && symbol.type.kind == CType::Kind::array &&
                    !symbol.type.count)
                    symbol.type.count = 1; // C11 6.9.2 tentative incomplete array
                define_global(unit, symbol);
            } else if (!symbol.definition && symbol.used) {
                unit.globals.push_back({symbol.name, 0, {}});
            }
        }
        if (diagnostics.error_count() != before)
            return std::nullopt;
        if (functions.size() + imports.size() + unit.globals.size() > max_symbols)
            throw std::length_error("native backend module symbol limit exceeded");

        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "archon-mir 1\nmodule " << functions.size() << ' ' << imports.size() << ' '
            << unit.globals.size() << '\n';
        for (const auto &function : functions)
            out << function;
        for (const auto &[name, type] : imports)
            write_import(out, name, type);
        for (const auto &global : unit.globals) {
            out << "global " << global.name << ' ' << global.words;
            if (global.data.empty())
                out << " zero\n";
            else {
                out << " init";
                for (auto word : global.data)
                    out << ' ' << word;
                out << '\n';
            }
        }
        out << "end\n";
        auto mir = out.str();
        if (mir.size() > max_mir)
            throw std::length_error("native MachineIR text limit exceeded");
        return LoweredUnit{std::move(mir), std::move(unit.locals), std::move(unit.data_assembly)};
    } catch (const std::length_error &err) {
        unit.error(at, err.what(), "cephyr.limit");
    } catch (const std::logic_error &err) {
        if (diagnostics.error_count() == before)
            unit.error(at, err.what(), "cephyr.frontend.internal");
    }
    return std::nullopt;
}

} // namespace cephyr
