#include <cephyr/lower.hpp>

#ifdef CEPHYR_WITH_MOONSTONE_ANALYSIS
#include "moonstone_analysis.hpp"
#endif
#include <cephyr/preprocess.hpp>

#include "Backend.h"

#include <AbstractSyntaxTree.h>
#include <wetstone/utils/Graph.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

namespace cephyr::detail {
namespace {

const std::vector<std::uint64_t> *find_member_path(const CType &type, std::string_view name)
{
    if (type.kind != CType::Kind::record || !type.record || !type.record->size)
        return nullptr;
    const auto path = type.record->member_paths.find(name);
    return path == type.record->member_paths.end() ? nullptr : &path->second;
}

constexpr std::size_t max_blocks = ARCHON_MIR_MAX_BLOCKS, max_values = 4096;
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

bool backend_signature(const Preprocessed &source, const CType &type)
{
    if (type.kind != CType::Kind::function || !type.element ||
        !(type.element->kind == CType::Kind::void_ || type.element->scalar() ||
          type.element->kind == CType::Kind::record) ||
        type.variadic || type.params.size() > ARCHON_MIR_MAX_PARAMS ||
        !record_abi_error(source, type).empty())
        return false;
    const auto params = abi_parameter_types(type);
    return std::all_of(params.begin(), params.end(), [](const CType &t) { return t.scalar(); });
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

// C11 6.2.7: retain known array bounds recursively and prefer a prototype.
CType composite_linkage_type(const CType &prior, CType next)
{
    if (!next.enum_identity)
        next.enum_identity = prior.enum_identity;
    if (next.kind == CType::Kind::array && !next.count)
        next.count = prior.count;
    if (next.kind == CType::Kind::function && prior.prototype && !next.prototype) {
        next.prototype = true;
        next.params = prior.params;
        next.variadic = prior.variadic;
    } else if (next.kind == CType::Kind::function && prior.prototype && next.prototype) {
        for (std::size_t i = 0; i < next.params.size(); ++i)
            next.params[i] = composite_linkage_type(prior.params[i], std::move(next.params[i]));
    }
    if (prior.element && next.element)
        next.element = std::make_shared<CType>(composite_linkage_type(*prior.element, *next.element));
    return next;
}

Symbol *linkage_entity(Unit &unit, const std::string &name, Node *declaration, Node *name_node,
                       const CType &type, bool internal)
{
    if (!symbol_name(name) || name.starts_with(reserved_prefix)) {
        unit.error(name_node, "unsupported or reserved C identifier for native output");
        return nullptr;
    }
    if (auto found = unit.symbols.find(name); found != unit.symbols.end()) {
        auto &symbol = found->second;
        if (internal != symbol.internal) {
            unit.error(name_node, "conflicting linkage for '" + name + "'");
            return nullptr;
        }
        if (!compatible(symbol.linkage_type, type)) {
            unit.error(name_node, "conflicting types for '" + name + "'");
            return nullptr;
        }
        return &symbol;
    }
    std::string native_name = internal ? std::string(reserved_prefix) + "local_" + name : name;
    if (!symbol_name(native_name)) {
        unit.error(name_node, "identifier exceeds the native backend's symbol limit");
        return nullptr;
    }
    Symbol symbol{type, std::move(native_name), declaration, nullptr, internal};
    symbol.linkage_type = type;
    return &unit.symbols.emplace(name, std::move(symbol)).first->second;
}

bool check_function_specifiers(Unit &unit, const std::string &name, unsigned flags, Node *at)
{
    if (unit.hosted && name == "main" && (flags & (FS_INLINE | FS_NORETURN))) {
        unit.error(at, "function specifiers are not allowed on hosted 'main'");
        return false;
    }
    return true;
}

// A declaration is made visible before lowering its initializer or body; a
// later name must not accidentally satisfy an earlier C use.
Symbol *declare(Unit &unit, Node *declaration, Node *name_node, CType type,
                unsigned flags, bool is_definition, bool has_initializer,
                std::optional<unsigned> alignment = std::nullopt)
{
    const std::string name = identifier(name_node);
    const bool function = type.kind == CType::Kind::function;
    const bool static_spec = flags & SCS_STATIC;
    const bool extern_spec = flags & SCS_EXTERN;
    if ((static_spec && extern_spec) || (flags & (SCS_AUTO | SCS_REGISTER))) {
        unit.error(name_node, "invalid file-scope storage class");
        return nullptr;
    }
    if (!type.valid())
        return nullptr;
    auto *prior = unit.find_file_symbol(name);
    const bool inherits = !static_spec && (extern_spec || function);
    const bool internal = static_spec || (inherits && prior && prior->internal);
    auto *entity = linkage_entity(unit, name, declaration, name_node, type, internal);
    if (!entity)
        return nullptr;
    auto &symbol = *entity;
    if (prior &&
        (!compatible(symbol.type, type) ||
         (function && symbol.type.prototype != type.prototype &&
         ((symbol.definition && !symbol.type.prototype && !type.params.empty()) ||
          (is_definition && !type.prototype && !symbol.type.params.empty()))))) {
        unit.error(name_node, "conflicting types for '" + name + "'");
        return nullptr;
    }
    if (symbol.definition && is_definition && (function || (symbol.initialized && has_initializer))) {
        unit.error(name_node, "redefinition of '" + name + "'");
        return nullptr;
    }
    symbol.linkage_type = composite_linkage_type(symbol.linkage_type, type);
    symbol.type = prior ? composite_linkage_type(symbol.type, std::move(type)) : std::move(type);
    symbol.file_scope = true;
    if (flags & FS_INLINE)
        symbol.inline_declaration = name_node;
    if (alignment)
        symbol.alignment_declarations.emplace_back(*alignment, name_node);
    // Without an initialized definition, an explicitly aligned tentative
    // definition can supply the storage alignment for the tentative group.
    if (is_definition && (!symbol.definition || has_initializer ||
                          (!symbol.initialized && !symbol.definition_alignment && alignment))) {
        symbol.definition = declaration;
        symbol.initialized = has_initializer;
        symbol.definition_alignment = alignment;
    }
    return &symbol;
}

bool check_linkage_alignment(Unit &unit, const Symbol &symbol) {
    if (symbol.alignment_declarations.empty())
        return true;
    if (symbol.definition && !symbol.definition_alignment) {
        unit.error(symbol.alignment_declarations.front().second,
                   "object definition must also specify _Alignas");
        return false;
    }
    const auto natural = align_of(symbol.type);
    unsigned first = 0;
    const TypeContext context(unit);
    for (const auto &[requested, at] : symbol.alignment_declarations) {
        if (!check_object_alignment(requested, natural, at, context))
            return false;
        const auto actual = std::max(requested, natural);
        if (!actual)
            continue; // The alignment of an incomplete type may remain unknown.
        if (first && actual != first) {
            unit.error(at, "conflicting _Alignas declarations");
            return false;
        }
        first = actual;
    }
    return true;
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
        op == "const" || op == "fconst" ||
        op == "frame_addr" || op == "global_addr" || op == "func_addr" ? 0 :
        op == "copy" || op == "sext" || op == "zext" || op == "trunc" ||
        op == "amd64_va_start" || op == "aarch64_va_start" ||
        op == "fload" || op == "fneg" || op == "fcast" || op == "sitofp" ||
        op == "uitofp" || op == "fptosi" || op == "fptoui" ||
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

std::string write_function(const Function &f, std::string_view name, std::size_t &module_instructions)
{
    const auto reachable = reachable_blocks(f.blocks);
    std::vector<Id> block_ids(f.blocks.size(), no_value), value_ids(f.values.size(), no_value);
    Id block_count = 0, value_count = 0;
    const auto params = abi_parameter_types(f.symbol.type, true).size();
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
    out << "function " << name << ' ' << mir_type(abi_result_type(*f.symbol.type.element)) << ' '
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
    const auto params = abi_parameter_types(type);
    out << "import " << name << ' ' << mir_type(abi_result_type(*type.element)) << ' ' << params.size();
    for (const auto &param : params)
        out << ' ' << mir_type(param);
    out << '\n';
}

bool integer_array(const CType &type)
{
    return type.kind == CType::Kind::array && type.element && type.element->integer();
}

class ObjectInitPlanner {
    using Path = std::vector<std::uint64_t>;
    struct Cursor {
        CType type;
        std::uint64_t offset;
    };
    struct Coverage {
        bool whole = false;
        std::map<std::uint64_t, Coverage> children;
    };

public:
    const TypeContext &context;
    std::vector<ObjectInitEntry> entries;

    ObjectInitPlanner(const TypeContext &c, bool direct) : context(c), direct(direct) {}

    bool initialize(CType &type, Node *node)
    {
        if (!initialize(Cursor{type, 0}, node, {}))
            return false;
        if (type.kind == CType::Kind::array && !type.count)
            type.count = inferred;
        if (direct && size_of(type))
            append_omitted({type, 0}, &coverage, node);
        return true;
    }

private:
    bool initialize(const Cursor &object, Node *node, const Path &path)
    {
        const AnalysisDepth guard(depth);
        const auto &[type, offset] = object;
        const bool braced = node->getKind() == cparser::NK_LIST;
        if (type.scalar() || (type.kind == CType::Kind::record && !braced)) {
            if (!cover(path, node))
                return false;
            entries.push_back({offset, type, node, {}});
            clear_active(path);
            return true;
        }

        const auto children = braced ? elements(node) : std::vector<Node *>{};
        Node *first = braced && !children.empty() ? children.front() : node;
        if (integer_array(type) && first->getKind() == cparser::NK_STRING_CONST) {
            if (children.size() > 1)
                return error(children[1], "too many elements after character-array string initializer");
            const auto literal = string_constant_type(first, context.source);
            auto element = *type.element;
            element.is_const = element.is_volatile = false;
            if (literal.element->is_plain_char ? element.bits != 8 :
                                                   !compatible(element, *literal.element))
                return error(first, "string literal encoding is incompatible with the array element type");
            const auto count = type.count ? type.count : literal.count;
            if (literal.count - 1 > count)
                return error(first, "string literal is too long for character array");
            auto bytes = string_constant_bytes(first);
            if (literal.count > count)
                bytes.resize(bytes.size() - literal.element->bits / 8); // Omit only the final code unit.
            if (!type.count)
                inferred = count;
            if (!reset(object, first, path) || !cover(path, first))
                return false;
            auto string_type = type;
            if (direct)
                string_type.count = count;
            entries.push_back({offset, std::move(string_type), first, std::move(bytes)});
            return true;
        }
        if (!braced)
            return error(node, "array initializer must be a brace-enclosed list or a character string");
        if (children.empty())
            return error(node, "aggregate initializer list must not be empty");

        if (!reset(object, node, path))
            return false;
        // Each pair of braces defines a current object (6.7.9p17–20).
        // Maintain the next relative path, descending into brace-elided
        // aggregates and advancing through enclosing aggregates on exhaustion.
        Path next{0};
        for (auto *initializer : children) {
            Cursor current = object;
            Path selected = path;
            Path relative;
            if (initializer->getKind() == cparser::NK_DESIGNATED_INIT) {
                overriding = true;
                auto *designated = static_cast<cparser::DesignatedInitASTNode *>(initializer);
                for (auto *designator : elements(designated->getDesignators())) {
                    if (designator->getKind() == cparser::NK_ARRAY_DESIGNATOR) {
                        const auto index = designated_array_index(current.type, designator);
                        if (!index || !step(current, selected, *index, designator))
                            return false;
                        relative.push_back(*index);
                    } else {
                        const auto *member = find_member_path(current.type, identifier(designator));
                        if (!member)
                            return error(designator, "member designator does not name a member of the current structure or union");
                        for (const auto index : *member) {
                            if (!step(current, selected, index, designator))
                                return false;
                            relative.push_back(index);
                        }
                    }
                }
                if (relative.empty())
                    return error(initializer, "designation must select a subobject");
                initializer = designated->getInit();
            } else {
                if (next.empty())
                    return error(initializer, "too many elements in aggregate initializer");
                relative = next;
                for (auto index : relative)
                    if (!step(current, selected, index, initializer))
                        return false;
            }
            while (!whole_object(current.type, initializer)) {
                if (!step(current, selected, 0, initializer))
                    return false;
                relative.push_back(0);
            }
            if (!initialize(current, initializer, selected))
                return false;
            if (path.empty() && type.kind == CType::Kind::array && !type.count)
                inferred = std::max(inferred, relative.front() + 1);
            next = advance(object, std::move(relative));
        }
        return true;
    }

    static CType child_type(const CType &parent, std::uint64_t index)
    {
        if (parent.kind == CType::Kind::record)
            return qualified_member_type(parent, parent.record->members[index]);
        auto type = *parent.element;
        type.is_const |= parent.is_const;
        type.is_volatile |= parent.is_volatile;
        return type;
    }

    Coverage *covered(const Path &path, bool insert)
    {
        auto *node = &coverage;
        for (auto index : path) {
            if (node->whole)
                return node; // An ancestor initialized the entire subobject.
            if (insert) {
                node = &node->children[index];
            } else {
                const auto found = node->children.find(index);
                if (found == node->children.end())
                    return nullptr;
                node = &found->second;
            }
        }
        return node;
    }

    bool cover(const Path &path, Node *at)
    {
        if (!direct)
            return true;
        auto &node = *covered(path, true);
        if (node.whole || !node.children.empty())
            return error(at, "overlapping designators in volatile automatic initialization are not supported");
        node.whole = true;
        return true;
    }

    void append_omitted(const Cursor &object, const Coverage *explicit_init, Node *at)
    {
        const auto &[type, offset] = object;
        if (!explicit_init) {
            entries.push_back({offset, type, at, {}, true});
            return;
        }
        if (explicit_init->whole)
            return;
        if (type.kind == CType::Kind::array) {
            const auto stride = *size_of(*type.element);
            const auto gap = [&](std::uint64_t first, std::uint64_t end) {
                if (first < end) {
                    auto missing = type;
                    missing.count = end - first;
                    entries.push_back({offset + first * stride, std::move(missing), at, {}, true});
                }
            };
            std::uint64_t next = 0;
            for (const auto &[index, child] : explicit_init->children) {
                gap(next, index);
                append_omitted({child_type(type, index), offset + index * stride}, &child, at);
                next = index + 1;
            }
            gap(next, type.count);
        } else if (type.record->is_union) {
            // Only the designated (or first) member is initialized.
            const auto &[index, child] = *explicit_init->children.begin();
            append_omitted({child_type(type, index), offset}, &child, at);
        } else {
            for (std::size_t i = 0; i < type.record->members.size(); ++i) {
                const auto &member = type.record->members[i];
                if (member.flexible_array())
                    break;
                const auto found = explicit_init->children.find(i);
                append_omitted({child_type(type, i), offset + member.offset},
                               found == explicit_init->children.end() ? nullptr : &found->second, at);
            }
        }
    }

    static Path advance(const Cursor &root, Path path)
    {
        std::vector<CType> parents;
        auto current = root.type;
        for (auto index : path) {
            parents.push_back(current);
            current = child_type(current, index);
        }
        while (!path.empty()) {
            auto &last = path.back();
            const auto &parent = parents.back();
            if (parent.kind == CType::Kind::array ? (!parent.count || last + 1 < parent.count) :
                (!parent.record->is_union && last + 1 < parent.record->members.size() &&
                 !parent.record->members[last + 1].flexible_array())) {
                ++last;
                return path;
            }
            path.pop_back();
            parents.pop_back();
        }
        return {};
    }

    bool step(Cursor &object, Path &path, std::uint64_t index, Node *at)
    {
        if (path.size() >= 512)
            throw std::length_error("C initializer nesting limit exceeded");
        const auto &type = object.type;
        if (type.kind == CType::Kind::array && type.element) {
            const auto stride = size_of(*type.element);
            if (!stride || !*stride)
                return error(at, "array initializer requires a complete element type");
            if (type.count && index >= type.count)
                return error(at, "array designator index is outside the array bound");
            if (index >= (std::numeric_limits<std::uint64_t>::max() - object.offset) / *stride)
                return error(at, "array initializer exceeds the object size limit");
            object.offset += index * *stride;
        } else if (type.kind == CType::Kind::record && type.record && type.record->size) {
            if (index >= type.record->members.size())
                return error(at, "too many elements in aggregate initializer");
            const auto &member = type.record->members[index];
            if (member.flexible_array())
                return error(at, "a flexible array member cannot be initialized");
            if (type.record->is_union) {
                if (const auto active = active_unions.find(path);
                    active != active_unions.end() && active->second != index &&
                    !reset(object, at, path, true))
                    return false;
                active_unions[path] = index;
            }
            object.offset += member.offset;
        } else {
            return error(at, "initializer requires a complete aggregate type");
        }
        object.type = child_type(type, index);
        path.push_back(index);
        return true;
    }

    std::optional<std::uint64_t> designated_array_index(const CType &type, Node *designator)
    {
        if (type.kind != CType::Kind::array) {
            error(designator, "array designator requires an array");
            return {};
        }
        auto *index = static_cast<cparser::ArrayDesignatorASTNode *>(designator)->getIndex();
        const auto constant = integer_constant(index, context);
        if (!constant || !constant->type.integer()) {
            error(index, "array designator index must be an integer constant expression");
            return {};
        }
        const auto value = normalize(constant->value, constant->type);
        if (constant->type.is_signed && std::bit_cast<std::int64_t>(value) < 0) {
            error(index, "array designator index must be nonnegative");
            return {};
        }
        return value;
    }

    bool whole_object(const CType &type, Node *node) const
    {
        if (!type.aggregate() || node->getKind() == cparser::NK_LIST ||
            (integer_array(type) && node->getKind() == cparser::NK_STRING_CONST))
            return true;
        if (type.kind != CType::Kind::record)
            return false;
        Diagnostics probe;
        auto source = expression_type(node, context.with_probe(probe));
        auto destination = type;
        source.is_const = destination.is_const = false;
        source.is_volatile = destination.is_volatile = false;
        return compatible(source, destination);
    }

    void clear_active(const Path &path)
    {
        for (auto it = active_unions.lower_bound(path);
             it != active_unions.end() && it->first.size() >= path.size() &&
             std::equal(path.begin(), path.end(), it->first.begin());)
            it = active_unions.erase(it);
    }

    bool reset(const Cursor &object, Node *node, const Path &path, bool switch_union = false)
    {
        // Re-initializing a complete subaggregate also zeroes its omitted
        // subobjects; switching union members discards the previous member.
        // Initially the root is zeroed by the caller, but it still needs a
        // reset when a later initializer switches between union members.
        if (direct) {
            const auto *prior = covered(path, false);
            if (switch_union || (prior && (prior->whole || !prior->children.empty())))
                return error(node, "overlapping designators in volatile automatic initialization are not supported");
        } else if (overriding && (switch_union || !path.empty())) {
            entries.push_back({object.offset, object.type, node, {}, true});
        }
        clear_active(path);
        return true;
    }

    bool error(Node *node, std::string_view message)
    {
        context.diagnostics.error("cephyr.aggregate.initializer", message,
                                  location(context.source, node));
        return false;
    }

    unsigned depth = 0;
    std::uint64_t inferred = 0;
    bool overriding = false;
    const bool direct;
    Coverage coverage;
    std::map<Path, std::uint64_t> active_unions;
};

} // namespace

std::string_view record_abi_error(const Preprocessed &source, const CType &signature)
{
    const auto check = [&](const CType &type) -> std::string_view {
        if (type.extended_real())
            return "by-value long double parameters and results are not supported";
        if (type.kind != CType::Kind::record)
            return {};
        // Keep language-side anonymous layouts out of the native ABI until
        // interoperability is verified, including through named/array members.
        if (type.record && type.record->contains_anonymous_member)
            return "by-value aggregate ABI does not support records containing anonymous members";
        if (!signature.prototype || signature.variadic)
            return "by-value aggregate ABI requires a fixed prototype";
        if (!source.native_rv64_record_abi)
            return "by-value aggregate ABI is only supported on the bundled RV64IM LP64 backend";
        const auto size = size_of(type);
        if (!size || !*size)
            return "by-value aggregate ABI requires a complete structure or union";
        if (align_of(type) > 8)
            return "by-value aggregate ABI does not support records aligned above 8 bytes";

        constexpr std::string_view unsupported =
            "by-value aggregate ABI requires integer or pointer members, without volatile members or flexible arrays";
        std::set<const RecordLayout *> visited;
        std::vector<const CType *> layout{&type};
        while (!layout.empty()) {
            const auto &t = *layout.back();
            layout.pop_back();
            if (t.is_volatile)
                return unsupported;
            if (t.integer() || t.pointer())
                continue;
            if (t.kind == CType::Kind::array && t.count && t.element) {
                layout.push_back(t.element.get());
                continue;
            }
            if (t.kind != CType::Kind::record || !t.record || !t.record->size ||
                t.record->contains_flexible_array)
                return unsupported;
            if (visited.insert(t.record).second)
                for (const auto &member : t.record->members)
                    layout.push_back(&member.type);
        }
        return {};
    };
    if (signature.element)
        if (const auto error = check(*signature.element); !error.empty())
            return error;
    for (const auto &parameter : signature.params)
        if (const auto error = check(parameter); !error.empty())
            return error;
    const auto result = signature.element ? record_abi(*signature.element) : RecordABI::scalar;
    std::size_t slots = signature.params.size() + (result == RecordABI::indirect ? 1 :
                                                   result == RecordABI::pair ? 2 : 0);
    bool aggregate = result != RecordABI::scalar;
    for (const auto &parameter : signature.params) {
        const auto abi = record_abi(parameter);
        aggregate |= abi != RecordABI::scalar;
        slots += abi == RecordABI::pair;
    }
    if (aggregate && slots > ARCHON_MIR_MAX_PARAMS)
        return "by-value aggregate ABI exceeds the native argument slot limit";
    return {};
}

RecordABI record_abi(const CType &type)
{
    if (type.kind != CType::Kind::record)
        return RecordABI::scalar;
    const auto size = size_of(type).value_or(0);
    return size <= 8 ? RecordABI::word : size <= 16 ? RecordABI::pair : RecordABI::indirect;
}

CType abi_result_type(const CType &type)
{
    switch (record_abi(type)) {
    case RecordABI::word: return integer_type(64, false);
    case RecordABI::pair:
    case RecordABI::indirect: return CType{CType::Kind::void_};
    case RecordABI::scalar: return type;
    }
    throw std::logic_error("unknown record ABI class");
}

CType record_pair_buffer_type()
{
    CType type;
    type.kind = CType::Kind::array;
    type.element = std::make_shared<const CType>(integer_type(64, false));
    type.count = 2;
    return type;
}

std::vector<CType> abi_parameter_types(const CType &signature, bool pair_body)
{
    std::vector<CType> params;
    const auto result = record_abi(*signature.element);
    if (result == RecordABI::indirect)
        params.push_back(pointer_type(*signature.element));
    for (const auto &type : signature.params) {
        switch (record_abi(type)) {
        case RecordABI::scalar: params.push_back(type); break;
        case RecordABI::indirect: params.push_back(pointer_type(type)); break;
        case RecordABI::pair: params.push_back(integer_type(64, false)); [[fallthrough]];
        case RecordABI::word: params.push_back(integer_type(64, false)); break;
        }
    }
    if (pair_body && result == RecordABI::pair)
        params.push_back(pointer_type(record_pair_buffer_type()));
    return params;
}

namespace {

// MIR's integer ABI currently has one result register. Keep the second word
// out of generic MIR, including its call clobber and SSA machinery: these
// local RV64 thunks translate between a two-word result and a private buffer.
// Scalarized arguments use the same slots at both interfaces, with any
// private parameters appended AFTER the public arguments.
class RecordPairThunk {
    std::ostringstream text;
    unsigned frame;

    void move_stack_argument(unsigned index)
    {
        const unsigned offset = 8 * (index - 8);
        text << "\tld t0, " << frame + offset << "(sp)\n"
             << "\tsd t0, " << offset << "(sp)\n";
    }

public:
    RecordPairThunk(std::string_view name, bool local, unsigned args, unsigned outgoing,
                    unsigned private_bytes)
        : frame(((outgoing > 8 ? 8 * (outgoing - 8) : 0) + private_bytes + 15) & ~15u)
    {
        text.imbue(std::locale::classic());
        text << "\t.text\n\t.p2align 2\n\t." << (local ? "local " : "globl ") << name
             << "\n\t.type " << name << ", @function\n" << name
             << ":\n\taddi sp, sp, -" << frame << "\n\tsd ra, " << frame - 8 << "(sp)\n";
        static_assert(ARCHON_MIR_MAX_PARAMS <= 128, "thunk stack offsets must fit RV64 load/store immediates");
        for (unsigned index = 8; index < args; ++index)
            move_stack_argument(index);
    }

    std::string definition(std::string_view entry, std::string_view body, unsigned args)
    {
        if (args < 8) {
            text << "\taddi a" << args << ", sp, " << frame - 24 << '\n';
        } else {
            text << "\taddi t0, sp, " << frame - 24 << "\n\tsd t0, "
                 << 8 * (args - 8) << "(sp)\n";
        }
        text << "\tcall " << body << "\n\tld a0, " << frame - 24 << "(sp)\n\tld a1, "
             << frame - 16 << "(sp)\n";
        return finish(entry);
    }

    std::string call(std::string_view name, unsigned args)
    {
        const auto get_private_arg = [&](unsigned index, std::string_view reg) {
            if (index < 8) {
                text << "\tmv " << reg << ", a" << index << '\n';
            } else {
                const unsigned offset = 8 * (index - 8);
                text << "\tld " << reg << ", " << frame + offset << "(sp)\n";
            }
        };
        get_private_arg(args, "t2");
        get_private_arg(args + 1, "t0");
        text << "\tsd t0, " << frame - 16 << "(sp)\n\tjalr t2\n"
             << "\tld t0, " << frame - 16 << "(sp)\n\tsd a0, 0(t0)\n\tsd a1, 8(t0)\n";
        return finish(name);
    }

private:
    std::string finish(std::string_view name)
    {
        text << "\tld ra, " << frame - 8 << "(sp)\n\taddi sp, sp, " << frame
             << "\n\tret\n\t.size " << name << ", .-" << name << '\n';
        return text.str();
    }
};

} // namespace

std::string Unit::require_record_pair_call(const CType &type)
{
    auto params = abi_parameter_types(type);
    const auto args = params.size();
    if (const auto found = record_pair_calls.find(args); found != record_pair_calls.end()) {
        // Preserve actual scalar MIR types for verification. Distinct call
        // signatures of the same word count may share the machine thunk.
        for (const auto &[name, signature] : signatures)
            if (name.starts_with(found->second + "_") &&
                std::equal(params.begin(), params.end(), signature.params.begin(),
                           [](const CType &a, const CType &b) { return mir_type(a) == mir_type(b); }))
                return name;
    }
    const auto shim = std::string(reserved_prefix) + "record_pair_call_" + std::to_string(args);
    if (record_pair_calls.emplace(args, shim).second)
        data_assembly += RecordPairThunk(shim, true, args, args, 16).call(shim, args);
    const auto alias = shim + "_" + std::to_string(signatures.size());
    data_assembly += "\t.set " + alias + ", " + shim + '\n';
    CType signature = type;
    signature.element = std::make_shared<const CType>(CType{CType::Kind::void_});
    params.push_back(pointer_type(type));
    params.push_back(pointer_type(record_pair_buffer_type()));
    signature.params = std::move(params);
    signatures.emplace_back(alias, std::move(signature));
    return alias;
}

std::string Unit::add_record_pair_definition(const Symbol &symbol)
{
    const auto body = std::string(reserved_prefix) + "record_pair_body_" + std::to_string(signatures.size());
    const auto args = abi_parameter_types(symbol.type).size();
    signatures.emplace_back(symbol.name, symbol.type); // func_addr uses the ABI entry point.
    data_assembly += RecordPairThunk(symbol.name, symbol.internal, args, args + 1, 24)
                         .definition(symbol.name, body, args);
    return body;
}

std::optional<std::vector<ObjectInitEntry>> plan_object_initializer(
    const TypeContext &context, CType &type, Node *node, bool direct)
{
    ObjectInitPlanner planner(context, direct);
    if (!planner.initialize(type, node))
        return std::nullopt;
    return std::move(planner.entries);
}

CType read_record_type(Node *node, const TypeContext &context)
{
    const bool structure = node->getKind() == cparser::NK_STRUCT_TYPE;
    const auto [identity, body] = [&]() -> std::pair<const void *, Node *> {
        if (structure) {
            auto *record = static_cast<cparser::StructTypeASTNode *>(node);
            return {record->getIdentity(), record->getBody()};
        }
        auto *record = static_cast<cparser::UnionTypeASTNode *>(node);
        return {record->getIdentity(), record->getBody()};
    }();
    auto &layout = context.unit.record_layouts[identity ? identity : node];
    CType type;
    type.kind = CType::Kind::record;
    type.record = &layout;
    if (!present(body) || layout.size || layout.defining)
        return type;

    layout.is_union = !structure;
    layout.defining = true;
    struct FinishDefinition {
        RecordLayout &layout;
        ~FinishDefinition() { layout.defining = false; }
    } finish{layout};
    const auto error = [&](Node *where, std::string_view message) {
        context.diagnostics.error("cephyr.record.layout", message, location(context.source, where));
        return CType{};
    };
    const auto round_up = [](std::uint64_t size, unsigned alignment) -> std::optional<std::uint64_t> {
        const auto mask = static_cast<std::uint64_t>(alignment - 1);
        if (size > std::numeric_limits<std::uint64_t>::max() - mask)
            return {};
        return (size + mask) & ~mask;
    };
    std::uint64_t extent = 0;
    unsigned max_alignment = 1;
    bool contains_flexible_array = false;
    bool contains_anonymous_member = false;
    std::vector<RecordMember> members;
    decltype(RecordLayout::member_paths) paths;
    for (auto *item : elements(body)) {
        if (item->getKind() == cparser::NK_STATIC_ASSERT) {
            const auto errors = context.diagnostics.error_count();
            check_static_assert(context, item);
            if (context.diagnostics.error_count() != errors)
                return {};
            continue;
        }
        if (item->getKind() != cparser::NK_FIELD_DECL)
            return error(item, "unsupported record member declaration");
        auto *field = static_cast<cparser::FieldDeclASTNode *>(item);
        auto *where = present(field->getName()) ? field->getName() : item;
        if (structure && contains_flexible_array)
            return error(where, "a flexible array member must be the last member of its structure");
        auto name = identifier(field->getName());
        auto member_type = read_type(field->getType(), context, true);
        if (!member_type.valid())
            return {};
        const bool flexible = member_type.kind == CType::Kind::array && !member_type.count;
        if (flexible) {
            if (!structure || paths.empty())
                return error(where,
                             "a flexible array member requires a structure with a preceding named member");
            if (!member_type.element || !size_of(*member_type.element))
                return error(where, "a flexible array member requires a complete element type");
            contains_flexible_array = true;
        }
        if (member_type.kind == CType::Kind::record && member_type.record->contains_flexible_array) {
            if (structure)
                return error(where, "a structure member cannot contain a flexible array member");
            contains_flexible_array = true;
        }
        // The incomplete array has an aligned offset but contributes no
        // elements to the fixed representation (6.7.2.1p18).
        const auto size = flexible ? std::optional<std::uint64_t>{0} : size_of(member_type);
        const auto natural = align_of(member_type);
        if (!size || (!flexible && !*size) || !natural)
            return error(where, "record member must have a complete object type");
        const auto requested = alignment_specifiers(field->getAlignments(), context);
        if (!requested || !check_object_alignment(*requested, natural, where, context))
            return {};
        const auto alignment = std::max(natural, *requested);
        const auto offset = structure ? round_up(extent, alignment) : std::optional<std::uint64_t>{0};
        if (!offset || *size > std::numeric_limits<std::uint64_t>::max() - *offset)
            return error(where, "record layout exceeds the object size limit");
        extent = std::max(extent, *offset + *size);
        max_alignment = std::max(max_alignment, alignment);
        const auto add_path = [&](const std::string &visible, const std::vector<std::uint64_t> &suffix) {
            auto [it, inserted] = paths.try_emplace(visible);
            if (inserted) {
                it->second.push_back(members.size());
                it->second.insert(it->second.end(), suffix.begin(), suffix.end());
            }
            return inserted;
        };
        if (name.empty()) {
            contains_anonymous_member = true;
            for (const auto &[visible, suffix] : member_type.record->member_paths)
                if (!add_path(visible, suffix))
                    return error(where, "duplicate record member introduced by anonymous structure or union: " + visible);
        } else if (!add_path(name, {})) {
            return error(where, "duplicate record member: " + name);
        }
        const auto *value_type = &member_type;
        while (value_type->kind == CType::Kind::array)
            value_type = value_type->element.get();
        if (value_type->kind == CType::Kind::record)
            contains_anonymous_member |= value_type->record->contains_anonymous_member;
        members.push_back({std::move(name), std::move(member_type), *offset});
    }
    if (paths.empty())
        return error(node, "record must contain at least one named member");
    const auto size = round_up(extent, max_alignment);
    if (!size)
        return error(node, "record layout exceeds the object size limit");
    layout.members = std::move(members);
    layout.member_paths = std::move(paths);
    for (const auto &member : layout.members) {
        layout.has_const_member |= object_has_qualifier(member.type, &CType::is_const);
        layout.has_volatile_member |= object_has_qualifier(member.type, &CType::is_volatile);
    }
    layout.alignment = max_alignment;
    layout.size = *size;
    layout.contains_flexible_array = contains_flexible_array;
    layout.contains_anonymous_member = contains_anonymous_member;
    return type;
}

std::optional<MemberAccess> find_member(const CType &type, std::string_view name)
{
    const auto *path = find_member_path(type, name);
    if (!path)
        return {};
    MemberAccess access{type, 0};
    for (const auto index : *path) {
        const auto &member = access.type.record->members[index];
        access.offset += member.offset;
        access.type = qualified_member_type(access.type, member);
    }
    return access;
}

CType qualified_member_type(const CType &record, const RecordMember &member)
{
    auto type = member.type;
    type.is_const |= record.is_const;
    type.is_volatile |= record.is_volatile;
    return type;
}

bool object_has_qualifier(const CType &type, bool CType::*qualifier)
{
    if (type.*qualifier)
        return true;
    if (type.kind == CType::Kind::array && type.element)
        return object_has_qualifier(*type.element, qualifier);
    if (type.kind != CType::Kind::record || !type.record)
        return false;
    return qualifier == &CType::is_const ? type.record->has_const_member :
        type.record->has_volatile_member;
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

Symbol *Unit::find_file_symbol(std::string_view name)
{
    auto found = symbols.find(std::string(name));
    return found != symbols.end() && found->second.file_scope ? &found->second : nullptr;
}

TypeBinding TypeContext::lookup(std::string_view name, Node *reference) const
{
    const std::string key(name);
    for (auto scope = prototype; scope; scope = scope->parent)
        if (const auto found = scope->parameters.find(key); found != scope->parameters.end())
            return found->second;
    if (function) {
        if (const auto linked = function->find_linked(key, reference))
            return {linked->type};
        if (const auto local = function->find_local(key))
            return {local->type, local->register_object};
        return {};
    }
    const auto *symbol = unit.find_file_symbol(key);
    return symbol ? TypeBinding{symbol->type} : TypeBinding{};
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
    // resolves its alias to the actual function. AMD64 also classifies FP
    // slots and sets the SysV vector-register count in its normal emitter.
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
    auto &assertion = *static_cast<cparser::StaticAssertASTNode *>(node);
    const auto count = context.diagnostics.error_count();
    const auto condition = integer_constant(assertion.getExpr(), context);
    const auto error = [&](std::string_view text) {
        context.diagnostics.error("cephyr.static_assert", text, location(context.source, node));
    };
    if (!condition) {
        if (context.diagnostics.error_count() == count)
            error("_Static_assert requires an integer constant expression");
    } else if (normalize(condition->value, condition->type) == 0) {
        std::string description = "static assertion failed";
        const auto *message = assertion.getMessage();
        auto text = message->getBytes();
        // C11 only requires the basic source characters in the diagnostic.
        for (const auto unit : message->getWideUnits())
            if (unit > 0 && unit < 0x80)
                text.push_back(static_cast<char>(unit));
        if (!text.empty())
            description += ": " + text;
        error(description);
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

Id Function::emit(const CType &type, std::string_view op, std::string operands, Node *origin)
{
    if (!open())
        throw std::logic_error("instruction emitted into a closed block");
    const auto id = value(type);
    blocks[current].instructions.push_back({std::string(op), std::move(operands), id, origin});
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

LValue Function::local(const CType &type, Node *at, unsigned requested_alignment)
{
    const auto size = size_of(type);
    const auto alignment = std::max(align_of(type), requested_alignment);
    if (!size || !*size || *size > std::numeric_limits<int>::max() ||
        !alignment || alignment > max_object_alignment) {
        error(at, "local object has incomplete or unsupported size or alignment");
        return {};
    }
    const auto index = locals.size();
    locals.emplace_back(static_cast<unsigned>(*size), alignment);
    // The stack slot exists for the whole function regardless of when its
    // source declaration or initializer is reached.
    const auto address = value(pointer_type(type));
    entry_addresses.push_back({"frame_addr", std::to_string(index), address, at});
    return {type, address};
}

Id offset_address(Function &function, Id base, std::uint64_t offset, const CType &pointee)
{
    if (!offset)
        return base;
    const Id amount = function.constant(offset, integer_type(64, false));
    return function.emit(pointer_type(pointee), "ptr_add",
                         std::to_string(base) + " " + std::to_string(amount));
}

namespace {

template<class MemoryStep>
void memory_range(Function &function, Id dest, Id src, std::uint64_t start,
                  std::uint64_t end, std::uint64_t width, MemoryStep step)
{
    const auto index_type = integer_type(64, false);
    if ((end - start) / width < 12) {
        for (auto position = start; position < end; position += width)
            step(function, dest, src, position ? function.constant(position, index_type) : no_value, width);
        return;
    }
    const auto begin = function.constant(start, index_type);
    const auto increment = function.constant(width, index_type);
    const auto limit = function.constant(end, index_type);
    const auto loop = function.block();
    const auto exit = function.block();
    const auto position = function.param(loop, index_type);
    function.jump(loop, {begin});
    function.select(loop);
    step(function, dest, src, position, width);
    const auto next = function.emit(index_type, "add",
                                    std::to_string(position) + " " + std::to_string(increment));
    const auto done = function.emit(integer_type(), "eq",
                                    std::to_string(next) + " " + std::to_string(limit));
    function.branch(done, exit, loop, {}, {next});
    function.select(exit);
}

Id byte_address(Function &function, Id base, Id offset)
{
    if (offset == no_value)
        return base;
    return function.emit(pointer_type(integer_type(8, false)), "ptr_add",
                         std::to_string(base) + " " + std::to_string(offset));
}

void store_chunk(Function &function, Id dest, Id data, unsigned width)
{
    const char *op = width == 8 ? "store" : width == 4 ? "store32" :
                     width == 2 ? "store16" : "store8";
    function.emit_void(op, std::to_string(dest) + " " + std::to_string(data) + " 0");
}

void copy_chunk(Function &f, Id dest, Id src, Id offset, unsigned width)
{
    const char *op = width == 8 ? "load" : width == 4 ? "load_u32" :
                     width == 2 ? "load_u16" : "load_u8";
    const auto in = byte_address(f, src, offset);
    const auto data = f.emit(integer_type(width * 8, false), op, std::to_string(in) + " 0");
    store_chunk(f, byte_address(f, dest, offset), data, width);
}

bool has_volatile_union_member(const CType &type, unsigned depth = 0)
{
    const AnalysisDepth guard(depth);
    if (type.kind == CType::Kind::array)
        return type.count && has_volatile_union_member(*type.element, depth);
    if (type.kind != CType::Kind::record || !type.record->has_volatile_member)
        return false;
    if (type.record->is_union)
        return true;
    for (const auto &member : type.record->members)
        if (has_volatile_union_member(member.type, depth))
            return true;
    return false;
}

void zero_padding(Function &f, Id dest, const CType &type, std::uint64_t start, std::uint64_t end)
{
    if (start == end)
        return;
    auto width = std::min(align_of(type), 8u);
    while ((start | end) & (width - 1))
        width /= 2;
    zero_object(f, offset_address(f, dest, start, integer_type(8, false)), end - start, width);
}

// The aggregate volatile access policy (C11 5.1.2.3, 6.7.3): visit fixed
// structure members/array elements in increasing address order, without
// widening accesses across member boundaries or reading/writing structure
// padding. A union's active member is not tracked, so a whole volatile union
// with no explicitly volatile members uses aligned representation chunks.
// With no source, initialize wholly omitted subobjects as at static duration
// (6.7.9p10,p19), selecting a union's first named member and zeroing padding.
// Zero representation is scalar zero/null on the native targets.
void transfer_subobjects(Function &f, Id dest, Id src, const CType &type, unsigned depth = 0)
{
    const AnalysisDepth guard(depth);
    if (type.kind == CType::Kind::array) {
        const auto stride = *size_of(*type.element);
        memory_range(f, dest, src, 0, stride * type.count, stride,
                     [&](Function &fn, Id out, Id in, Id offset, std::uint64_t) {
            transfer_subobjects(fn, byte_address(fn, out, offset),
                                in == no_value ? no_value : byte_address(fn, in, offset),
                                *type.element, depth);
        });
    } else if (type.kind == CType::Kind::record && (!type.record->is_union || src == no_value)) {
        std::uint64_t end = 0;
        for (const auto &member : type.record->members) {
            if (member.flexible_array())
                break;
            if (src == no_value)
                zero_padding(f, dest, type, end, member.offset);
            transfer_subobjects(f, offset_address(f, dest, member.offset, member.type),
                                src == no_value ? no_value :
                                    offset_address(f, src, member.offset, member.type),
                                member.type, depth);
            if (src == no_value)
                end = member.offset + *size_of(member.type);
            if (type.record->is_union)
                break;
        }
        if (src == no_value)
            zero_padding(f, dest, type, end, *size_of(type));
    } else if (type.extended_real()) {
        if (src == no_value) {
            zero_object(f, dest, 16, 16);
        } else {
            // Transfer only the x87 payload for member-wise (volatile)
            // copies; the trailing six bytes are not part of the value.
            copy_chunk(f, dest, src, no_value, 8);
            const auto offset = f.constant(8);
            copy_chunk(f, dest, src, offset, 2);
        }
    } else if (src == no_value) {
        store_chunk(f, dest, f.constant(0), static_cast<unsigned>(*size_of(type)));
    } else if (type.kind == CType::Kind::record) {
        memory_range(f, dest, src, 0, *size_of(type), std::min(align_of(type), 8u), copy_chunk);
    } else {
        copy_chunk(f, dest, src, no_value, static_cast<unsigned>(*size_of(type)));
    }
}

} // namespace

void zero_object(Function &function, Id dest, std::uint64_t size, unsigned alignment)
{
    alignment = std::min(alignment, 8u);
    const auto zero = function.constant(0);
    const auto fill = [](Function &f, Id out, Id data, Id offset, unsigned width) {
        store_chunk(f, byte_address(f, out, offset), data, width);
    };
    // C object size is a multiple of its alignment, as required for arrays
    // with adjacent elements of this type.
    memory_range(function, dest, zero, 0, size, alignment, fill);
}

void zero_initialized_subobject(Function &function, Id dest, const CType &type)
{
    transfer_subobjects(function, dest, no_value, type);
}

// The RV64 integer calling convention passes an aggregate up to 2 * XLEN
// in one or two consecutive integer slots. Splitting the representation at
// XLEN lets the existing backend pass the slots in registers or on the stack,
// including a first word in a7 with the second at 0(sp). The last slot's
// low bytes are laid out as in memory, not extended as a narrow C scalar.
// Never read or write past the end of the C object.
Id pack_record_abi(Function &function, Expr value, unsigned index)
{
    const auto type = integer_type(64, false);
    const auto start = 8 * index;
    const auto size = std::min<std::uint64_t>(8, *size_of(value.type) - start);
    const auto width = align_of(value.type);
    const char *load = width == 8 ? "load" : width == 4 ? "load_u32" :
                       width == 2 ? "load_u16" : "load_u8";
    Id word = no_value;
    for (unsigned offset = 0; offset < size; offset += width) {
        const Id address = offset_address(function, value.id, start + offset, value.type);
        const Id chunk = function.emit(type, load, std::to_string(address) + " 0");
        if (!offset) {
            word = chunk;
        } else {
            const Id shift = function.constant(8 * offset, type);
            const Id part = function.emit(type, "shl", std::to_string(chunk) + " " + std::to_string(shift));
            word = function.emit(type, "or", std::to_string(word) + " " + std::to_string(part));
        }
    }
    return word;
}

void unpack_record_abi(Function &function, LValue object, Id word, unsigned index)
{
    const auto type = integer_type(64, false);
    const auto start = 8 * index;
    const auto size = std::min<std::uint64_t>(8, *size_of(object.type) - start);
    const auto width = align_of(object.type);
    for (unsigned offset = 0; offset < size; offset += width) {
        Id part = word;
        if (offset) {
            const Id shift = function.constant(8 * offset, type);
            part = function.emit(type, "lshr", std::to_string(word) + " " + std::to_string(shift));
        }
        store_chunk(function, offset_address(function, object.address, start + offset, object.type), part, width);
    }
}

bool copy_record(Function &function, LValue target, Expr value, Node *at, bool initializing)
{
    if (!target.type.valid() || !value.type.valid())
        return false;
    auto target_type = target.type, source_type = value.type;
    target_type.is_const = source_type.is_const = false;
    target_type.is_volatile = source_type.is_volatile = false;
    const auto size = size_of(target_type);
    if (target_type.kind != CType::Kind::record || !size || !compatible(target_type, source_type)) {
        function.error(at, "record copy requires compatible complete structure or union types");
        return false;
    }
    if (!initializing && object_has_qualifier(target.type, &CType::is_const)) {
        function.error(at, "record assignment cannot modify a const-qualified member or object");
        return false;
    }
    if (object_has_qualifier(target.type, &CType::is_volatile) ||
        object_has_qualifier(value.type, &CType::is_volatile)) {
        if (has_volatile_union_member(target_type) || has_volatile_union_member(source_type)) {
            function.error(at, "whole-object copy of a union with volatile members is not supported");
            return false;
        }
        // An evaluated record lvalue is first copied to a temporary by load().
        // Thus all source-object reads precede destination-object writes, even
        // with exact overlap, and an assignment result never rereads its lhs.
        // Both QML and native emission preserve all of these memory operations,
        // just as for scalar volatile accesses.
        transfer_subobjects(function, target.address, value.id, target_type);
        return true;
    }
    // C11 6.5.16.1p3 only defines exact overlap between compatible
    // operands. Copy the object representation without libc or misaligned
    // word loads, preserving the active member's bytes in a union.
    memory_range(function, target.address, value.id, 0, *size, std::min(align_of(target_type), 8u),
                 copy_chunk);
    return true;
}

bool append_abi_argument(Function &function, Expr value, Node *at, std::vector<Id> &args)
{
    switch (record_abi(value.type)) {
    case RecordABI::scalar:
        args.push_back(abi_value(function, value));
        break;
    case RecordABI::word:
        args.push_back(pack_record_abi(function, value));
        break;
    case RecordABI::pair:
        args.push_back(pack_record_abi(function, value, 0));
        args.push_back(pack_record_abi(function, value, 1));
        break;
    case RecordABI::indirect: {
        // The ABI permits the callee to overwrite the referenced argument.
        // Always snapshot here, even when the source is a nonvolatile lvalue.
        const auto temporary = function.local(value.type, at);
        if (temporary.address == no_value || !copy_record(function, temporary, value, at, true))
            return false;
        args.push_back(temporary.address);
        break;
    }
    }
    return true;
}

std::optional<LValue> Function::find_local(std::string_view name) const
{
    const std::string key(name);
    for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope)
        if (const auto found = scope->find(key); found != scope->end())
            return found->second;
    return std::nullopt;
}

std::optional<LValue> Function::find_linked(std::string_view name, Node *reference) const
{
    auto binding = find_local(name);
    if (binding && !binding->linked)
        return std::nullopt;
    if (!binding) {
        auto *entry = unit.find_file_symbol(name);
        if (!entry)
            return std::nullopt;
        binding = LValue{entry->type, no_value, {}, false, entry};
    }
    if (reference && symbol.inline_only && binding->linked->internal &&
        !symbol.inline_internal_reference)
        symbol.inline_internal_reference = reference;
    return binding;
}

void declare_block_linkage(Function &function, Node *declaration, Node *name_node, CType type,
                           unsigned flags, std::optional<unsigned> alignment)
{
    auto &unit = function.unit;
    const std::string name = identifier(name_node);
    const auto current = function.scopes.back().find(name);
    if (current != function.scopes.back().end() && !current->second.linked) {
        unit.error(name_node, "redeclaration of '" + name + "' in the same block");
        return;
    }
    if (type.kind == CType::Kind::function && !check_function_specifiers(unit, name, flags, name_node))
        return;
    const auto prior = flags & DS_HIDES_NONLINKAGE ? std::nullopt : function.find_linked(name);
    auto *entity = linkage_entity(unit, name, declaration, name_node, type,
                                  prior && prior->linked->internal);
    if (!entity)
        return;
    entity->linkage_type = composite_linkage_type(entity->linkage_type, type);
    if (prior)
        type = composite_linkage_type(prior->type, std::move(type));
    if (alignment)
        entity->alignment_declarations.emplace_back(*alignment, name_node);
    if (flags & FS_INLINE)
        entity->inline_declaration = name_node;
    function.scopes.back().insert_or_assign(name, LValue{std::move(type), no_value, {}, false, entity});
}

bool Function::declare_local(std::string name, LValue storage, Node *at)
{
    // Block-scope objects and parameters are keyed by the lexer's canonical
    // spelling, not emitted as assembler symbols. Their storage uses frame
    // indices or compiler-generated names.
    if (name.empty()) {
        error(at, "local declaration has no identifier");
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
                                 Diagnostics &diagnostics, bool hosted, detail::MoonstoneAnalysis *analysis)
{
    using namespace detail;
    using namespace cparser;
#ifndef CEPHYR_WITH_MOONSTONE_ANALYSIS
    (void)analysis;
#endif

    Unit unit(source, diagnostics, hosted);
    std::vector<std::string> functions;
    std::set<std::string> typedef_names;
    std::set<std::string> ordinary_function_declarations;
    std::size_t instruction_count = 0;
    const auto before = diagnostics.error_count();
    Node *at = root;
    try {
        const auto declarations = elements(root);
        // Determine C11 6.7.4's TU-wide property without making any later
        // declaration visible to a function body or initializer.
        for (Node *node : declarations) {
            if (!present(node) || node->getKind() != NK_FUNCTION_DECL)
                continue;
            auto &decl = *static_cast<FunctionDeclASTNode *>(node);
            if (!(decl.getFlags() & FS_INLINE) || (decl.getFlags() & SCS_EXTERN))
                ordinary_function_declarations.insert(identifier(decl.getName()));
        }
        for (Node *node : declarations) {
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
                if (!name.empty() && unit.find_file_symbol(name))
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
                const auto name = identifier(decl.getName());
                if (!check_function_specifiers(unit, name, flags, decl.getName()))
                    continue;
                auto *symbol = declare(unit, node, decl.getName(), std::move(type), flags, definition, false);
                if (!symbol || !definition)
                    continue;
                symbol->inline_only = !symbol->internal && !ordinary_function_declarations.contains(name);
                if (const auto error = record_abi_error(unit.source, symbol->type); !error.empty()) {
                    unit.error(decl.getName(), error, "cephyr.abi.aggregate");
                    continue;
                }
                  if (symbol->type.variadic && !unit.source.native_scalar_stdarg()) {
                      unit.error(decl.getName(), "variadic definitions require a bundled SysV AMD64 or AAPCS64 backend");
                     continue;
                 }
                 auto signature = symbol->type;
                 // Named scalar slots have the same ABI as a fixed signature.
                 // Do not admit aggregate slots in this variadic profile.
                 if (signature.variadic && signature.element &&
                     (signature.element->scalar() || signature.element->kind == CType::Kind::void_) &&
                     std::all_of(signature.params.begin(), signature.params.end(),
                                 [](const CType &param) { return param.scalar(); }))
                     signature.variadic = false;
                 if (!backend_signature(unit.source, signature)) {
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
                if (symbol->inline_internal_reference)
                    unit.error(symbol->inline_internal_reference,
                               "external inline definition references an identifier with internal linkage");
                // Always use the external definition, even for same-TU calls.
                // The inline-only body was checked but supplies no native definition.
                if (symbol->inline_only)
                    continue;
                if (errors == diagnostics.error_count()) {
#ifdef CEPHYR_WITH_MOONSTONE_ANALYSIS
                    if (analysis)
                        analysis->inspect(f);
#endif
                    const bool pair = record_abi(*symbol->type.element) == RecordABI::pair;
                    const auto name = pair ? unit.add_record_pair_definition(*symbol) : symbol->name;
                    functions.push_back(write_function(f, name, instruction_count));
                    if (symbol->internal || pair)
                        unit.locals.push_back(name);
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
                if (type.kind == CType::Kind::record && definition && !initialized &&
                    (flags & SCS_STATIC) && !size_of(type)) {
                    unit.error(decl.getName(), "internal tentative definition has incomplete structure or union type");
                    continue;
                }
                if (type.kind == CType::Kind::array && definition && !initialized &&
                    (flags & SCS_STATIC) && !type.count) {
                    const auto *prior = unit.find_file_symbol(identifier(decl.getName()));
                    if (!prior || prior->type.kind != CType::Kind::array || !prior->type.count) {
                        unit.error(decl.getName(),
                                   "tentative definition with internal linkage has incomplete array type");
                        continue;
                    }
                }
                const auto requested = alignment_specifiers(decl.getAlignments(), TypeContext(unit));
                if (!requested ||
                    !check_object_alignment(*requested, align_of(type), decl.getName(), TypeContext(unit)))
                    continue;
                const auto alignment = present(decl.getAlignments()) ? requested : std::nullopt;
                auto *symbol = declare(unit, node, decl.getName(), std::move(type), flags,
                                       definition, initialized, alignment);
                // Initializers see exactly the declarations visible here.
                // This also completes `char s[] = "...";` before any later
                // use of sizeof(s). Tentative definitions are finalized below.
                if (symbol && definition && initialized && define_global(unit, *symbol)) {
                    if (!compatible(symbol->linkage_type, symbol->type))
                        unit.error(decl.getName(), "conflicting types for '" + identifier(decl.getName()) + "'");
                    else
                        symbol->linkage_type = composite_linkage_type(symbol->linkage_type, symbol->type);
                }
                continue;
            }
            if (node->getKind() == NK_STRUCT_TYPE || node->getKind() == NK_UNION_TYPE ||
                node->getKind() == NK_ENUMERAL_TYPE) {
                (void)read_type(node, TypeContext(unit), true);
                continue;
            }
            unit.error(node, "unsupported file-scope C construct");
        }

        std::vector<std::pair<std::string, CType>> imports = unit.signatures;
        for (auto &[source_name, symbol] : unit.symbols) {
            at = symbol.declaration;
            if (!check_linkage_alignment(unit, symbol))
                continue;
            if (symbol.type.kind == CType::Kind::function) {
                if (symbol.inline_declaration && !symbol.internal && !symbol.definition)
                    unit.error(symbol.inline_declaration,
                               "external inline function '" + source_name + "' has no definition in this translation unit");
                if ((!symbol.definition || symbol.inline_only) && symbol.used) {
                    if (symbol.internal)
                        unit.error(at, "internal function '" + source_name + "' has no definition");
                    else if (const auto error = record_abi_error(unit.source, symbol.linkage_type); !error.empty())
                        unit.error(at, error, "cephyr.abi.aggregate");
                    else if (backend_signature(unit.source, symbol.linkage_type))
                        imports.emplace_back(symbol.name, symbol.linkage_type);
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
