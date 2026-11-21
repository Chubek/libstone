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
        op == "const" || op == "fconst" ||
        op == "frame_addr" || op == "global_addr" || op == "func_addr" ? 0 :
        op == "copy" || op == "sext" || op == "zext" || op == "trunc" ||
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

class ObjectInitPlanner {
    using Path = std::vector<std::uint64_t>;
    struct Cursor {
        CType type;
        std::uint64_t offset;
    };

public:
    const TypeContext &context;
    std::vector<ObjectInitEntry> entries;

    explicit ObjectInitPlanner(const TypeContext &c) : context(c) {}

    bool initialize(CType &type, Node *node)
    {
        if (!initialize(Cursor{type, 0}, node, {}))
            return false;
        if (type.kind == CType::Kind::array && !type.count)
            type.count = inferred;
        return true;
    }

private:
    bool initialize(const Cursor &object, Node *node, const Path &path)
    {
        const AnalysisDepth guard(depth);
        const auto &[type, offset] = object;
        const bool braced = node->getKind() == cparser::NK_LIST;
        if (type.scalar() || (type.kind == CType::Kind::record && !braced)) {
            entries.push_back({offset, type, node, {}});
            clear_active(path);
            return true;
        }

        const auto children = braced ? elements(node) : std::vector<Node *>{};
        Node *first = braced && !children.empty() ? children.front() : node;
        if (character_array(type) && first->getKind() == cparser::NK_STRING_CONST) {
            if (children.size() > 1)
                return error(children[1], "too many elements after character-array string initializer");
            auto bytes = string_constant(first, context.source, context.diagnostics);
            if (!bytes)
                return false;
            const auto count = type.count ? type.count : bytes->size();
            if (bytes->size() - 1 > count)
                return error(first, "string literal is too long for character array");
            if (bytes->size() > count)
                bytes->pop_back(); // C11 6.7.9p14: only omit the final terminator.
            if (!type.count)
                inferred = count;
            reset(object, first, path);
            entries.push_back({offset, type, first, std::move(bytes)});
            return true;
        }
        if (!braced)
            return error(node, "array initializer must be a brace-enclosed list or a character string");
        if (children.empty())
            return error(node, "aggregate initializer list must not be empty");

        reset(object, node, path);
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
                    const auto index = designated_index(current.type, designator);
                    if (!index || !step(current, selected, *index, designator))
                        return false;
                    relative.push_back(*index);
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
                    active != active_unions.end() && active->second != index)
                    reset(object, at, path, true);
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

    std::optional<std::uint64_t> designated_index(const CType &type, Node *designator)
    {
        if (designator->getKind() == cparser::NK_ARRAY_DESIGNATOR) {
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
        if (const auto *member = find_member(type, identifier(designator)))
            return static_cast<std::uint64_t>(member - type.record->members.data());
        error(designator, "member designator does not name a member of the current structure or union");
        return {};
    }

    bool whole_object(const CType &type, Node *node) const
    {
        if (!type.aggregate() || node->getKind() == cparser::NK_LIST ||
            (character_array(type) && node->getKind() == cparser::NK_STRING_CONST))
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

    void reset(const Cursor &object, Node *node, const Path &path, bool switch_union = false)
    {
        // Re-initializing a complete subaggregate also zeroes its omitted
        // subobjects; switching union members discards the previous member.
        // Initially the root is zeroed by the caller, but it still needs a
        // reset when a later initializer switches between union members.
        if (overriding && (switch_union || !path.empty()))
            entries.push_back({object.offset, object.type, node, {}, true});
        clear_active(path);
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
    std::map<Path, std::uint64_t> active_unions;
};

} // namespace

std::optional<std::vector<ObjectInitEntry>> plan_object_initializer(
    const TypeContext &context, CType &type, Node *node)
{
    ObjectInitPlanner planner(context);
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
    std::vector<RecordMember> members;
    std::set<std::string> names;
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
        if (structure && contains_flexible_array)
            return error(field->getName(), "a flexible array member must be the last member of its structure");
        auto name = identifier(field->getName());
        if (name.empty())
            return error(item, "unnamed and anonymous record members are not supported");
        if (!names.emplace(name).second)
            return error(field->getName(), "duplicate record member");
        auto member_type = read_type(field->getType(), context, true);
        if (!member_type.valid())
            return {};
        const bool flexible = member_type.kind == CType::Kind::array && !member_type.count;
        if (flexible) {
            if (!structure || members.empty())
                return error(field->getName(),
                             "a flexible array member requires a structure with a preceding named member");
            if (!member_type.element || !size_of(*member_type.element))
                return error(field->getName(), "a flexible array member requires a complete element type");
            contains_flexible_array = true;
        }
        if (member_type.kind == CType::Kind::record && member_type.record->contains_flexible_array) {
            if (structure)
                return error(field->getName(), "a structure member cannot contain a flexible array member");
            contains_flexible_array = true;
        }
        // The incomplete array has an aligned offset but contributes no
        // elements to the fixed representation (6.7.2.1p18).
        const auto size = flexible ? std::optional<std::uint64_t>{0} : size_of(member_type);
        const auto alignment = align_of(member_type);
        if (!size || (!flexible && !*size) || !alignment)
            return error(field->getName(), "record member must have a complete object type");
        // All supported constituents follow the bundled little-endian LP64
        // targets' natural layout. Bit-fields and explicit alignment must not
        // fall through to this algorithm.
        if ((alignment & (alignment - 1)) != 0)
            return error(field->getName(), "unsupported record member alignment");
        const auto offset = structure ? round_up(extent, alignment) : std::optional<std::uint64_t>{0};
        if (!offset || *size > std::numeric_limits<std::uint64_t>::max() - *offset)
            return error(field->getName(), "record layout exceeds the object size limit");
        extent = std::max(extent, *offset + *size);
        max_alignment = std::max(max_alignment, alignment);
        members.push_back({std::move(name), std::move(member_type), *offset});
    }
    if (members.empty())
        return error(node, "record must contain at least one named member");
    const auto size = round_up(extent, max_alignment);
    if (!size)
        return error(node, "record layout exceeds the object size limit");
    layout.members = std::move(members);
    for (const auto &member : layout.members) {
        layout.has_const_member |= object_has_qualifier(member.type, &CType::is_const);
        layout.has_volatile_member |= object_has_qualifier(member.type, &CType::is_volatile);
    }
    layout.alignment = max_alignment;
    layout.size = *size;
    layout.contains_flexible_array = contains_flexible_array;
    return type;
}

const RecordMember *find_member(const CType &type, std::string_view name)
{
    if (type.kind != CType::Kind::record || !type.record || !type.record->size)
        return nullptr;
    for (const auto &member : type.record->members)
        if (member.name == name)
            return &member;
    return nullptr;
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
    auto &assertion = *static_cast<cparser::StaticAssertASTNode *>(node);
    const auto count = context.diagnostics.error_count();
    const auto condition = integer_constant(assertion.getExpr(), context);
    const auto message = string_constant(assertion.getMessage(), context.source, context.diagnostics);
    const auto error = [&](std::string_view text) {
        context.diagnostics.error("cephyr.static_assert", text, location(context.source, node));
    };
    if (!condition) {
        if (context.diagnostics.error_count() == count)
            error("_Static_assert requires an integer constant expression");
    } else if (message && normalize(condition->value, condition->type) == 0) {
        std::string description = "static assertion failed";
        std::string_view literal(*message);
        if (!literal.empty() && literal.back() == '\0')
            literal.remove_suffix(1);
        if (!literal.empty())
            description += ": " + std::string(literal);
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

using MemoryStep = void (*)(Function &, Id dest, Id src, Id offset, unsigned width);

void memory_range(Function &function, Id dest, Id src, std::uint64_t start,
                  std::uint64_t end, unsigned width, MemoryStep step)
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
        function.error(at, "volatile object access is not supported");
        return false;
    }
    // C11 6.5.16.1p3 only defines exact overlap between compatible
    // operands. Copy the object representation without libc or misaligned
    // word loads, preserving the active member's bytes in a union.
    memory_range(function, target.address, value.id, 0, *size, std::min(align_of(target_type), 8u),
                 [](Function &f, Id dest, Id src, Id offset, unsigned width) {
        const char *op = width == 8 ? "load" : width == 4 ? "load_u32" :
                         width == 2 ? "load_u16" : "load_u8";
        const auto in = byte_address(f, src, offset);
        const auto data = f.emit(integer_type(width * 8, false), op, std::to_string(in) + " 0");
        store_chunk(f, byte_address(f, dest, offset), data, width);
    });
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
#ifdef CEPHYR_WITH_MOONSTONE_ANALYSIS
                    if (analysis)
                        analysis->inspect(f);
#endif
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
                if (type.kind == CType::Kind::record && definition && !initialized &&
                    (flags & SCS_STATIC) && !size_of(type)) {
                    unit.error(decl.getName(), "internal tentative definition has incomplete structure or union type");
                    continue;
                }
                auto *symbol = declare(unit, node, decl.getName(), std::move(type), flags, definition, initialized);
                // Initializers see exactly the declarations visible here.
                // This also completes `char s[] = "...";` before any later
                // use of sizeof(s). Tentative definitions are finalized below.
                if (symbol && definition && initialized)
                    define_global(unit, *symbol);
                continue;
            }
            if (node->getKind() == NK_STRUCT_TYPE || node->getKind() == NK_UNION_TYPE) {
                (void)read_type(node, TypeContext(unit), true);
                continue;
            }
            if (node->getKind() == NK_ENUMERAL_TYPE)
                continue;
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
