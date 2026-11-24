#include "interface.hpp"

#include "declarations.hpp"
#include "../FactIR.hpp"
#include "moonstone_analysis.hpp"

#include <cephyr/preprocess.hpp>

#include <AbstractSyntaxTree.h>
#include <SatieCDCL.hpp>
#include <SatieTheoryUtils.hpp>
#include <wetstone/abstract-interp/KnownBits.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <locale>
#include <sstream>

namespace cephyr::detail {

thread_local const TagObserver *TagObserver::active_ = nullptr;

TagObserver::TagObserver(const Unit &unit, Observe observe)
    : unit_(unit), observe_(std::move(observe)), previous_(active_)
{
    active_ = this;
}

TagObserver::~TagObserver()
{
    active_ = previous_;
}

void TagObserver::notify(const Unit &unit, Node *node, const CType &type)
{
    if (active_ && &active_->unit_ == &unit)
        active_->observe_(node, type);
}

} // namespace cephyr::detail

namespace cephyr {
namespace {

using namespace detail;
using namespace cparser;

const char *truth(bool value) { return value ? "true" : "false"; }

void quoted(std::ostream &out, std::string_view text)
{
    static constexpr char hex[] = "0123456789abcdef";
    out << '"';
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\')
            out << '\\' << static_cast<char>(c);
        else if (c < ' ' || c == 0x7f)
            out << "\\u00" << hex[c >> 4] << hex[c & 15];
        else
            out << static_cast<char>(c);
    }
    out << '"';
}

void optional_name(std::ostream &out, std::string_view name)
{
    if (name.empty())
        out << "none";
    else
        quoted(out, name);
}

template<class T>
void known(std::ostream &out, std::optional<T> value)
{
    if (value)
        out << *value;
    else
        out << "unknown";
}

const char *integer_name(const CType &type)
{
    if (type.is_plain_char)
        return "char";
    switch (type.rank) {
    case 0: return "bool";
    case 1: return type.is_signed ? "signed-char" : "unsigned-char";
    case 2: return type.is_signed ? "short" : "unsigned-short";
    case 3: return type.is_signed ? "int" : "unsigned-int";
    case 4: return type.is_signed ? "long" : "unsigned-long";
    case 5: return type.is_signed ? "long-long" : "unsigned-long-long";
    }
    throw std::logic_error("unrecognized C integer rank");
}

struct Tag {
    std::string name;
    CType type;
    Node *body;
};

struct Declaration {
    std::string name;
    CType alias;
    Symbol *symbol = nullptr;
    std::vector<unsigned> flags;
    std::vector<std::string> parameter_names;
    bool definition_names = false;
    std::vector<Node *> sites;
};

// A closed C fragment: unsigned expressions and reads of fully initialized
// automatic arrays that never escape. All other memory models are unknown.
class ReturnProof {
    using Bits = std::vector<satie::Lit>;
    using Encoder = satie::theory::Encoder;
    using KnownBits = wetstone::ai::KnownBits;
    const TypeContext context;
    std::vector<std::string> names;
    std::map<std::string, std::vector<std::uint32_t>> arrays;
    std::vector<satie::Lit> unsafe;
    Node *expression = nullptr;
    unsigned budget = 0;

    static bool uint_type(const CType &type)
    {
        return type.integer() && !type.is_signed && !type.enum_identity &&
               !type.is_volatile && type.rank == 3 && type.bits == 32;
    }

    static Bits constant(Encoder &enc, std::uint32_t value)
    {
        Bits bits;
        for (unsigned i = 0; i < 32; ++i)
            bits.push_back((value >> i) & 1 ? enc.const_true() : enc.const_false());
        return bits;
    }

    static Bits select(Encoder &enc, satie::Lit condition, const Bits &a, const Bits &b)
    {
        Bits result;
        for (unsigned i = 0; i < 32; ++i)
            result.push_back(enc.lor2(enc.land2(condition, a[i]), enc.land2(-condition, b[i])));
        return result;
    }

    static satie::SolveResult check(Encoder enc, Bits counterexample)
    {
        enc.add_clause(std::move(counterexample));
        satie::CNF cnf;
        for (auto &clause : enc.take_clauses())
            cnf.add_clause(std::move(clause));
        satie::CDCLSolver solver(std::move(cnf));
        solver.set_conflict_budget(128);
        return solver.solve();
    }

    std::optional<satie::Lit> condition(Node *node, Encoder &enc,
                                        const std::vector<Bits> &inputs, satie::Lit path)
    {
        if (present(node) && node->getKind() == NK_LT_EXPR) {
            auto *comparison = static_cast<LtExprASTNode *>(node);
            const auto a = encode(comparison->getLhs(), enc, inputs, path);
            const auto b = encode(comparison->getRhs(), enc, inputs, path);
            if (!a || !b)
                return {};
            return enc.ult(*a, *b);
        }
        auto value = encode(node, enc, inputs, path);
        if (!value)
            return {};
        auto result = enc.const_false();
        for (auto bit : *value)
            result = enc.lor2(result, bit);
        return result;
    }

    bool bind_array(Node *node)
    {
        if (node->getKind() != NK_VAR_DECL || arrays.size() >= 4)
            return false;
        auto *var = static_cast<VarDeclASTNode *>(node);
        auto *type = var->getType();
        const auto name = identifier(var->getName());
        if (name.empty() || std::ranges::find(names, name) != names.end() || arrays.contains(name) ||
            !present(type) || type->getKind() != NK_ARRAY_TYPE ||
            ((node->getFlags() | type->getFlags()) & (SCS_STATIC | SCS_EXTERN | SCS_REGISTER | TQ_VOLATILE)) ||
            present(var->getAlignments()))
            return false;
        auto *array = static_cast<ArrayTypeASTNode *>(type);
        auto *bound = array->getExpr();
        if (!present(bound) || bound->getKind() != NK_INTEGER_CONST ||
            !uint_type(read_type(array->getElementType(), context, true)))
            return false;
        const auto count = integer_constant(bound, context);
        auto *init = var->getInit();
        if (!count || !count->value || count->value > 8 || !present(init) || init->getKind() != NK_LIST)
            return false;
        auto values = elements(init);
        if (values.size() != count->value)
            return false;
        std::vector<std::uint32_t> data;
        for (auto *item : values) {
            if (item->getKind() != NK_INTEGER_CONST)
                return false;
            const auto value = integer_constant(item, context);
            if (!value || !uint_type(value->type))
                return false;
            data.push_back(static_cast<std::uint32_t>(value->value));
        }
        arrays.emplace(name, std::move(data));
        return true;
    }

    std::optional<Bits> encode(Node *node, Encoder &enc,
                               const std::vector<Bits> &inputs, satie::Lit path,
                               KnownBits *known = nullptr)
    {
        if (!present(node) || ++budget > 24)
            return {};
        if (known)
            *known = KnownBits::top(32);
        switch (node->getKind()) {
        case NK_IDENT_NODE:
            for (std::size_t i = 0; i < names.size(); ++i)
                if (names[i] == identifier(node))
                    return inputs[i];
            return {};
        case NK_INTEGER_CONST: {
            const auto value = integer_constant(node, context);
            if (!value || !uint_type(value->type))
                return {};
            if (known)
                *known = KnownBits::constant(value->value, 32);
            return constant(enc, static_cast<std::uint32_t>(value->value));
        }
        case NK_COND_EXPR: {
            auto *choice = static_cast<CondExprASTNode *>(node);
            const auto test = condition(choice->getCondition(), enc, inputs, path);
            if (!test)
                return {};
            KnownBits left, right;
            const auto a = encode(choice->getThenClause(), enc, inputs, enc.land2(path, *test), known ? &left : nullptr);
            const auto b = encode(choice->getElseClause(), enc, inputs, enc.land2(path, -*test), known ? &right : nullptr);
            if (!a || !b)
                return {};
            if (known)
                *known = left.join(right);
            return select(enc, *test, *a, *b);
        }
        case NK_ARRAY_REF: {
            auto *access = static_cast<ArrayRefASTNode *>(node);
            const auto array = arrays.find(identifier(access->getExpr()));
            if (array == arrays.end())
                return {};
            const auto index = encode(access->getIndex(), enc, inputs, path);
            if (!index)
                return {};
            // [i,i+1) selects one integer index. An out-of-bounds lookup has
            // no C result; the placeholder result bits below are used only
            // after proving no such lookup can be evaluated.
            const auto &data = array->second;
            if (known) {
                *known = KnownBits::bottom(32);
                for (auto element : data)
                    *known = known->join(KnownBits::constant(element, 32));
            }
            const auto within = enc.ult(*index, constant(enc, data.size()));
            unsafe.push_back(enc.land2(path, -within));
            auto value = constant(enc, 0);
            for (std::size_t i = data.size(); i-- > 0;)
                value = select(enc, enc.ult(*index, constant(enc, i + 1)), constant(enc, data[i]), value);
            return value;
        }
        default:
            break;
        }
        Node *lhs, *rhs;
        switch (node->getKind()) {
#define BIN(kind, Class) case kind: { \
            auto *binary = static_cast<Class *>(node); \
            lhs = binary->getLhs(); rhs = binary->getRhs(); break; \
        }
        BIN(NK_BIT_AND_EXPR, BitAndExprASTNode)
        BIN(NK_BIT_IOR_EXPR, BitIorExprASTNode)
        BIN(NK_BIT_XOR_EXPR, BitXorExprASTNode)
#undef BIN
        default:
            return {};
        }
        KnownBits left, right;
        const auto a = encode(lhs, enc, inputs, path, known ? &left : nullptr);
        const auto b = encode(rhs, enc, inputs, path, known ? &right : nullptr);
        if (!a || !b)
            return {};
        if (known)
            *known = node->getKind() == NK_BIT_AND_EXPR ? left.bit_and(right) :
                     node->getKind() == NK_BIT_IOR_EXPR ? left.bit_or(right) :
                                                          left.bit_xor(right);
        Bits bits;
        for (unsigned i = 0; i < 32; ++i) {
            bits.push_back(node->getKind() == NK_BIT_AND_EXPR ? enc.land2((*a)[i], (*b)[i]) :
                           node->getKind() == NK_BIT_IOR_EXPR ? enc.lor2((*a)[i], (*b)[i]) :
                                                               enc.lxor2((*a)[i], (*b)[i]));
        }
        return bits;
    }

public:
    explicit ReturnProof(Unit &unit) : context(unit) {}

    void write(std::ostream &out, const Symbol &symbol)
    {
        auto *node = symbol.definition;
        const auto &type = symbol.type;
        if (!present(node) || node->getKind() != NK_FUNCTION_DECL ||
            !type.prototype || type.variadic || type.params.size() > 8 ||
            !uint_type(*type.element) || !std::ranges::all_of(type.params, uint_type))
            return;
        auto *definition = static_cast<FunctionDeclASTNode *>(node);
        if (!definition->hasPrototype())
            return;
        auto *body = definition->getBody();
        if (!present(body) || body->getKind() != NK_COMPOUND_STMT)
            return;
        const auto statements = elements(static_cast<CompoundStmtASTNode *>(body)->getItems());
        if (statements.empty() || statements.size() > 5 ||
            statements.back()->getKind() != NK_RETURN_STMT)
            return;
        expression = static_cast<ReturnStmtASTNode *>(statements.back())->getExpr();
        if (!type.params.empty()) {
            const auto params = elements(definition->getPrms());
            if (params.size() != type.params.size())
                return;
            for (auto *param : params) {
                if (param->getKind() != NK_PARM_DECL)
                    return;
                auto *decl = static_cast<ParmDeclASTNode *>(param);
                if (!uint_type(read_parameter_type(decl->getType(), context, true)))
                    return;
                auto name = identifier(decl->getName());
                if (name.empty() || std::ranges::find(names, name) != names.end())
                    return;
                names.push_back(std::move(name));
            }
        }
        for (std::size_t i = 0; i + 1 < statements.size(); ++i)
            if (!bind_array(statements[i]))
                return;
        satie::theory::Encoder enc(1);
        std::vector<Bits> baseline(names.size(), Bits(32));
        for (auto &input : baseline)
            for (auto &bit : input)
                bit = enc.fresh_lit();
        KnownBits known;
        const auto value = encode(expression, enc, baseline, enc.const_true(), &known);
        if (!value)
            return;
        const auto memory = check(enc, unsafe);
        out << " (memory-safety (model closed-unsigned-expression-v2) (scope this-definition)"
               " (status " << (memory.unsatisfiable() ? "proved" : memory.satisfiable() ? "disproved" : "unknown")
            << ") (checked-accesses " << unsafe.size() << ") (solver satie))";
        if (!memory.unsatisfiable())
            return;
        Bits violations;
        for (unsigned i = 0; i < known.width; ++i) {
            if ((known.zero >> i) & 1)
                violations.push_back((*value)[i]);
            if ((known.one >> i) & 1)
                violations.push_back(-(*value)[i]);
        }
        if (!violations.empty() && check(enc, std::move(violations)).unsatisfiable())
            out << " (return-known-bits 1 (analysis wetstone) (solver satie) (scope this-definition)"
                   " (width " << known.width << ") (zero-mask " << known.zero
                << ") (one-mask " << known.one << "))";
        out << " (body-proof (model closed-unsigned-expression-v2) (scope this-definition)"
               " (memory-effects none-by-construction) (return-dependencies";
        for (std::size_t i = 0; i < names.size(); ++i) {
            satie::theory::Encoder query(1);
            auto a = baseline;
            for (auto &input : a)
                for (auto &bit : input)
                    bit = query.fresh_lit();
            auto b = a;
            for (auto &bit : b[i])
                bit = query.fresh_lit();
            budget = 0;
            unsafe.clear();
            const auto first = encode(expression, query, a, query.const_true());
            budget = 0;
            const auto second = encode(expression, query, b, query.const_true());
            Bits differences;
            for (unsigned bit = 0; bit < 32; ++bit)
                differences.push_back(query.lxor2((*first)[bit], (*second)[bit]));
            const auto result = check(std::move(query), std::move(differences));
            out << " (parameter " << i << ' '
                << (result.unsatisfiable() ? "independent" :
                    result.satisfiable() ? "may-influence" : "unknown") << ')';
        }
        out << ") (return-policies";
        for (unsigned width : {8u, 16u, 31u}) {
            const auto result = check(enc, Bits(value->begin() + width, value->end()));
            out << " (unsigned-width " << width << ' '
                << (result.unsatisfiable() ? "proved" : result.satisfiable() ? "disproved" : "unknown")
                << ')';
        }
        out << ") (solver satie))";
    }
};

class Projection {
    Unit &unit;
    std::vector<Tag> tags;
    std::map<const void *, unsigned> tag_ids; // lookup only; never iterated
    std::vector<Declaration> declarations;
    std::map<std::string, std::size_t> declaration_ids;

    static const void *identity(const CType &type)
    {
        return type.kind == CType::Kind::record ? type.record : type.enum_identity;
    }

    void observe_tag(Node *node, const CType &type)
    {
        Node *name;
        Node *body;
        switch (node->getKind()) {
        case NK_STRUCT_TYPE: {
            auto *tag = static_cast<StructTypeASTNode *>(node);
            name = tag->getName();
            body = tag->getBody();
            break;
        }
        case NK_UNION_TYPE: {
            auto *tag = static_cast<UnionTypeASTNode *>(node);
            name = tag->getName();
            body = tag->getBody();
            break;
        }
        case NK_ENUMERAL_TYPE: {
            auto *tag = static_cast<EnumeralTypeASTNode *>(node);
            name = tag->getName();
            body = tag->getBody();
            break;
        }
        default:
            throw std::logic_error("unexpected C tag node");
        }
        const auto [it, inserted] = tag_ids.try_emplace(identity(type), tags.size());
        if (inserted)
            tags.push_back({identifier(name), type, body});
        else if (present(body) && !present(tags[it->second].body))
            tags[it->second].body = body;
    }

    void put_alias(Node *node, std::string name, CType type)
    {
        if (name.empty())
            return; // The tag observer handles declarations without a typedef name.
        const auto [it, inserted] = declaration_ids.try_emplace(name, declarations.size());
        if (inserted) {
            declarations.push_back({std::move(name), std::move(type), nullptr, {}, {}, false, {node}});
        } else {
            auto &decl = declarations[it->second];
            decl.sites.push_back(node);
            auto &alias = decl.alias;
            if (!compatible(alias, type))
                unit.error(node, "conflicting types for typedef '" + name + "'");
            else
                alias = composite_type(alias, std::move(type));
        }
    }

    void put_symbol(Node *node, const std::string &name, Symbol &symbol, unsigned flags)
    {
        const auto [it, inserted] = declaration_ids.try_emplace(name, declarations.size());
        if (inserted)
            declarations.push_back({name, {}, &symbol, {}, {}, false, {}});
        auto &declaration = declarations[it->second];
        declaration.flags.push_back(flags);
        declaration.sites.push_back(node);
        if (symbol.type.kind != CType::Kind::function || declaration.definition_names)
            return;

        Node *parameters = nullptr;
        const bool definition = symbol.definition == node;
        if (node->getKind() == NK_FUNCTION_DECL) {
            parameters = static_cast<FunctionDeclASTNode *>(node)->getPrms();
        } else if (node->getKind() == NK_VAR_DECL) {
            auto *type = static_cast<VarDeclASTNode *>(node)->getType();
            if (present(type) && type->getKind() == NK_FUNCTION_TYPE)
                parameters = static_cast<FunctionTypeASTNode *>(type)->getPrms();
        }
        // Names are presentation metadata, not part of the composite type.
        const auto slots = elements(parameters);
        if (slots.size() != symbol.type.params.size())
            return;
        std::vector<std::string> names(slots.size());
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (slots[i]->getKind() == NK_PARM_DECL)
                names[i] = identifier(static_cast<ParmDeclASTNode *>(slots[i])->getName());
        // Preserve the names as a group, rather than mixing names from
        // different declarations (which might use the same name for
        // different parameters).
        if (definition || (declaration.parameter_names.empty() &&
            std::ranges::any_of(names, [](const std::string &name) { return !name.empty(); }))) {
            declaration.definition_names = definition;
            declaration.parameter_names = std::move(names);
        }
    }

    static void layout(std::ostream &out, const CType &type, unsigned min_align = 0)
    {
        out << "(size ";
        known(out, size_of(type));
        out << ") (align ";
        const auto alignment = std::max(align_of(type), min_align);
        known(out, alignment ? std::optional{alignment} : std::nullopt);
        out << ')';
    }

    static void integer(std::ostream &out, const CType &type)
    {
        out << "(integer " << integer_name(type) << " (rank " << type.rank
            << ") (bits " << type.bits << ") (signed " << truth(type.is_signed) << ") ";
        layout(out, type);
        out << ' ';
        integer_range(out, type, type.bits);
        out << ')';
    }

    static void integer_range(std::ostream &out, const CType &type, unsigned bits)
    {
        out << "(value-range ";
        // The reviewed targets use two's complement and at most 64 bits.
        if (bits == 0 || bits > 64) {
            out << "unknown)";
            return;
        }
        if (type.is_signed) {
            const auto magnitude = std::uint64_t{1} << (bits - 1);
            out << '-' << magnitude << ' ' << magnitude - 1;
        } else {
            out << "0 " << (bits == 64 ? std::numeric_limits<std::uint64_t>::max() :
                                         (std::uint64_t{1} << bits) - 1);
        }
        out << ')';
    }

    void write_sites(std::ostream &out, const Declaration &decl) const
    {
        out << " (declarations";
        for (auto *site : decl.sites) {
            Node *name = nullptr;
            Node *parameters = nullptr;
            bool definition = false;
            switch (site->getKind()) {
            case NK_FUNCTION_DECL: {
                auto *fn = static_cast<FunctionDeclASTNode *>(site);
                name = fn->getName();
                parameters = fn->getPrms();
                definition = present(fn->getBody());
                break;
            }
            case NK_TYPE_DECL:
                name = static_cast<TypeDeclASTNode *>(site)->getName();
                break;
            case NK_VAR_DECL: {
                auto *var = static_cast<VarDeclASTNode *>(site);
                name = var->getName();
                auto *type = var->getType();
                if (present(type) && type->getKind() == NK_FUNCTION_TYPE)
                    parameters = static_cast<FunctionTypeASTNode *>(type)->getPrms();
                definition = present(var->getInit());
                break;
            }
            default:
                break;
            }
            const auto where = location(unit.source, present(name) ? name : site);
            out << " (site (file ";
            if (where.line)
                quoted(out, where.file);
            else
                out << "unknown";
            out << ") (line ";
            known(out, where.line ? std::optional{where.line} : std::nullopt);
            out << ')';
            if (definition)
                out << " (definition true)";
            std::size_t index = 0;
            for (auto *slot : elements(parameters)) {
                const auto i = index++;
                if (slot->getKind() != NK_PARM_DECL)
                    continue;
                auto *param = static_cast<ParmDeclASTNode *>(slot);
                auto *type = param->getType();
                if (!present(type))
                    continue;
                const bool reg = (slot->getFlags() & SCS_REGISTER) != 0;
                if (type->getKind() != NK_ARRAY_TYPE) {
                    if (reg)
                        out << " (parameter " << i << " (storage register))";
                    continue;
                }
                auto *array = static_cast<ArrayTypeASTNode *>(type);
                out << " (parameter " << i << " (adjusted-from array)";
                if (reg)
                    out << " (storage register)";
                if (array->hasStarBound())
                    out << " (variable-bound true)";
                if (array->hasStaticBound()) {
                    const auto found = unit.array_bounds.find(array->getExpr());
                    out << " (requires-array-access ";
                    known(out, found != unit.array_bounds.end() && found->second && *found->second
                        ? found->second : std::nullopt);
                    out << ')';
                }
                const auto qualifiers = array->getBracketQualifiers();
                if (qualifiers) {
                    out << " (adjusted-pointer-qualifiers";
                    if (qualifiers & TQ_CONST) out << " const";
                    if (qualifiers & TQ_VOLATILE) out << " volatile";
                    if (qualifiers & TQ_RESTRICT) out << " restrict";
                    out << ')';
                }
                out << ')';
            }
            out << ')';
        }
        out << ')';
    }

    void write_type(std::ostream &out, const CType &type) const
    {
        const bool qualified = type.is_const || type.is_volatile || type.is_restrict;
        if (qualified) {
            out << "(qualified (qualifiers";
            if (type.is_const)
                out << " const";
            if (type.is_volatile)
                out << " volatile";
            if (type.is_restrict)
                out << " restrict";
            out << ") ";
        }
        switch (type.kind) {
        case CType::Kind::void_:
            out << "void";
            break;
        case CType::Kind::integer:
            if (type.enum_identity)
                out << "(tag-ref " << tag_ids.at(identity(type)) << ')';
            else
                integer(out, type);
            break;
        case CType::Kind::real:
            out << "(real " << (type.bits == 32 ? "float" : type.bits == 64 ? "double" : "long-double")
                << " (format " << (type.bits == 32 ? "binary32" : type.bits == 64 ? "binary64" :
                                    type.bits == 80 ? "x87" : "binary128")
                << ") (bits " << type.bits << ") ";
            layout(out, type);
            out << ')';
            break;
        case CType::Kind::pointer:
            out << "(pointer (bits " << type.bits << ") (category "
                << (type.function_pointer() ? "function" : "object") << ") ";
            layout(out, type);
            out << " (pointee ";
            write_type(out, *type.element);
            out << "))";
            break;
        case CType::Kind::array:
            out << "(array (count ";
            known(out, type.count ? std::optional{type.count} : std::nullopt);
            out << ") ";
            layout(out, type);
            out << " (element ";
            write_type(out, *type.element);
            out << "))";
            break;
        case CType::Kind::function:
            out << "(function-type (prototype " << truth(type.prototype)
                << ") (variadic " << truth(type.variadic) << ") (result ";
            write_type(out, *type.element);
            out << ") (parameters";
            for (const auto &param : type.params) {
                out << ' ';
                write_type(out, param);
            }
            out << "))";
            break;
        case CType::Kind::record:
            out << "(tag-ref " << tag_ids.at(identity(type)) << ')';
            break;
        case CType::Kind::invalid:
            throw std::logic_error("invalid projected C type");
        }
        if (qualified)
            out << ')';
    }

    void write_tag(std::ostream &out, std::size_t id, const Tag &tag) const
    {
        const auto *record = tag.type.record;
        const bool complete = record ? record->size != 0 : present(tag.body);
        out << "\n  (tag " << id << ' ' << (record ? (record->is_union ? "union" : "struct") : "enum") << ' ';
        optional_name(out, tag.name);
        out << " (complete " << truth(complete) << ')';
        if (complete && record) {
            out << " (layout ";
            layout(out, tag.type);
            out << ") (traits (const-member " << truth(record->has_const_member)
                << ") (volatile-member " << truth(record->has_volatile_member)
                << ") (contains-anonymous-member " << truth(record->contains_anonymous_member)
                << ") (contains-flexible-array " << truth(record->contains_flexible_array)
                << ") (contains-bit-field " << truth(record->contains_bit_field) << "))";
            for (const auto &member : record->members) {
                out << "\n    (field ";
                optional_name(out, member.name);
                out << " (offset " << member.offset << ')';
                if (const auto &bits = member.bit_field) {
                    out << " (bit-field (width " << bits->width << ") (shift " << bits->shift
                        << ") (container-offset " << member.offset - bits->byte_in_container << ") ";
                    integer_range(out, member.type, bits->width);
                    out << ')';
                }
                if (member.flexible_array())
                    out << " (flexible-array true)";
                out << " (type ";
                write_type(out, member.type);
                out << "))";
            }
        } else if (complete) {
            out << " (underlying ";
            integer(out, tag.type);
            out << ')';
            for (auto *node : elements(tag.body)) {
                const auto *enumerator = static_cast<EnumeratorDeclASTNode *>(node);
                out << "\n    (enumerator ";
                quoted(out, enumerator->getName());
                out << ' ' << std::bit_cast<std::int64_t>(unit.enumeration_constants.at(node)) << ')';
            }
        }
        out << ')';
    }

    void write_declaration(std::ostream &out, const Declaration &decl,
                           std::string_view calling_convention, unsigned &proof_budget) const
    {
        const auto *symbol = decl.symbol;
        const auto &type = symbol ? symbol->type : decl.alias;
        const bool function = type.kind == CType::Kind::function;
        out << "\n  (" << (!symbol ? "typedef" : function ? "function" : "object") << ' ';
        quoted(out, decl.name);
        if (symbol) {
            out << " (linkage " << (symbol->internal ? "internal" : "external") << ") (storage";
            for (const unsigned flags : decl.flags)
                out << ' ' << ((flags & SCS_STATIC) ? "static" : (flags & SCS_EXTERN) ? "extern" : "none");
            out << ") (definition "
                << (!symbol->definition ? "declaration" : symbol->inline_only ? "inline-only" :
                    !function && !symbol->initialized ? "tentative" : "defined") << ')';
            if (function) {
                out << " (calling-convention " << calling_convention << ") (parameter-names";
                for (std::size_t i = 0; i < type.params.size(); ++i) {
                    out << ' ';
                    optional_name(out, i < decl.parameter_names.size() ?
                        std::string_view(decl.parameter_names[i]) : std::string_view{});
                }
                out << ") (specifiers";
                if (std::any_of(decl.flags.begin(), decl.flags.end(),
                                [](unsigned flags) { return (flags & FS_INLINE) != 0; }))
                    out << " inline";
                const bool noreturn = std::any_of(decl.flags.begin(), decl.flags.end(),
                                [](unsigned flags) { return (flags & FS_NORETURN) != 0; });
                if (noreturn)
                    out << " noreturn";
                out << ')';
                out << " (call-contract (normal-return " << (noreturn ? "forbidden-by-declaration" : "unknown")
                    << ") (fixed-parameters " << (type.prototype ? std::to_string(type.params.size()) : "unknown")
                    << ") (variadic-tail " << (!type.prototype ? "unknown" : truth(type.variadic)) << "))";
                out << " (behavior (general-body-analysis not-performed)"
                       " (memory-read unknown) (memory-write unknown)"
                       " (allocation unknown) (deallocation unknown) (throws-or-traps unknown)"
                       " (synchronization unknown) (termination unknown)"
                       " (return-alias unknown) (return-nullability unknown)"
                       " (taint-inputs unknown) (taint-outputs unknown) (taint-transfer unknown))";
                if (symbol->definition && !noreturn && proof_budget) {
                    --proof_budget;
                    std::ostringstream proof;
                    proof.imbue(std::locale::classic());
                    ReturnProof{unit}.write(proof, *symbol);
                    if (proof.tellp() > 0)
                        out << std::move(proof).str();
                    else
                        out << " (memory-safety (status unknown))";
                } else
                    out << " (memory-safety (status unknown))";
            } else {
                const auto requested = symbol->alignment_declarations.empty() ? 0 :
                    symbol->alignment_declarations.front().first;
                out << " (layout ";
                layout(out, type, requested);
                out << ") (storage-duration static)";
            }
        }
        out << " (type ";
        write_type(out, type);
        out << ')';
        write_sites(out, decl);
        out << ')';
    }

public:
    explicit Projection(Unit &u) : unit(u) {}

    void resolve(Node *root, Node *&at)
    {
        TagObserver observer(unit, [this](Node *node, const CType &type) { observe_tag(node, type); });
        resolve_file_declarations(unit, root, at, DeclarationConsumer{
            true,
            [this](Node *node, std::string name, CType type) {
                put_alias(node, std::move(name), std::move(type));
            },
            [this](Node *node, const std::string &name, Symbol &symbol, unsigned flags) {
                put_symbol(node, name, symbol, flags);
            },
            [this](Node *node, const std::string &name, Symbol &symbol, unsigned flags) {
                put_symbol(node, name, symbol, flags);
                auto *decl = static_cast<VarDeclASTNode *>(node);
                return !present(decl->getInit()) || validate_interface_initializer(unit, symbol);
            },
        });

        for (auto &decl : declarations) {
            auto *symbol = decl.symbol;
            if (!symbol)
                continue;
            at = symbol->declaration;
            if (!check_linkage_alignment(unit, *symbol))
                continue;
            if (symbol->type.kind == CType::Kind::function) {
                if (symbol->inline_declaration && !symbol->internal && !symbol->definition)
                    unit.error(symbol->inline_declaration,
                               "external inline function '" + decl.name + "' has no definition in this translation unit");
            } else if (symbol->definition && !symbol->initialized) {
                at = symbol->definition;
                (void)finish_tentative_object(unit, *symbol);
            }
        }
    }

    void write_native_analysis(std::ostream &out, Node *root, const Target &target,
                               bool factir_enabled, bool moonstone) const
    {
        out << "\n  (native-analysis 1 (scope translation-unit)";
        if (!factir_enabled && !moonstone) {
            out << " (status disabled))";
            return;
        }
        if (std::ranges::none_of(declarations, [](const Declaration &decl) {
                return decl.symbol && decl.symbol->definition &&
                       decl.symbol->type.kind == CType::Kind::function;
            })) {
            out << " (status no-definitions))";
            return;
        }
        // Native ABI restrictions and MIR limits must not invalidate a valid
        // declaration projection. Keep this pass's diagnostics in its report.
        Diagnostics findings;
        detail::MoonstoneAnalysis *inspector = nullptr;
#ifdef CEPHYR_WITH_MOONSTONE_ANALYSIS
        detail::MoonstoneAnalysis inspection(findings);
        if (moonstone)
            inspector = &inspection;
#endif
        auto lowered = lower(root, unit.source, findings, unit.hosted, inspector);
        if (!lowered)
            out << " (status lowering-unavailable)";
        else if (!factir_enabled)
            out << " (status lowered) (factir disabled)";
        else {
            auto facts = factir::run(lowered->mir, target, unit.source.file, findings);
            if (!facts)
                out << " (status analysis-unavailable)";
            else if (facts->budget_exceeded)
                out << " (status budget-exceeded)";
            else {
                out << " (status analyzed) (factir-schema ";
                quoted(out, factir::schema);
                out << ") (relations ";
                quoted(out, facts->facts);
                out << ") (rewrites " << facts->rewrites << ") (smtlib ";
                quoted(out, facts->smtlib);
                out << ')';
            }
        }
        std::ostringstream messages;
        findings.render(messages);
        if (!messages.view().empty()) {
            out << " (diagnostics ";
            quoted(out, messages.view());
            out << ')';
        }
        out << ')';
    }

    std::string write(Node *root, const Target &target, bool factir_enabled, bool moonstone) const
    {
        const auto &abi = target.abi;
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "(interface 1\n  (target ";
        quoted(out, target.triple);
        out << ")\n  (data-model LP64)\n  (abi ";
        quoted(out, abi);
        out << ")\n  (analysis-model 1 (scope declarations bounded-return-proof native-mir)"
               " (facts c11-frontend) (unknown not-a-guarantee))"
               "\n  (target-properties (byte-bits 8) (byte-order little)"
               " (pointer-bits 64) (integer-representation twos-complement)"
               " (hosted " << truth(unit.hosted) << "))";
        for (std::size_t i = 0; i < tags.size(); ++i)
            write_tag(out, i, tags[i]);
        const auto convention = abi == "SysVAMD64" ? "sysv" : abi == "AAPCS64" ? "aapcs64" : "lp64";
        unsigned proof_budget = 128;
        for (const auto &decl : declarations)
            write_declaration(out, decl, convention, proof_budget);
        write_native_analysis(out, root, target, factir_enabled, moonstone);
        out << ")\n";
        return std::move(out).str();
    }
};

} // namespace

std::optional<std::string> project_interface(cparser::ASTNode *root, const Preprocessed &source,
                                             Diagnostics &diagnostics, bool hosted,
                                             const Target &target, bool factir_enabled, bool moonstone)
{
    const auto before = diagnostics.error_count();
    Unit unit(source, diagnostics, hosted);
    auto *at = root;
    try {
        Projection projection(unit);
        projection.resolve(root, at);
        if (diagnostics.error_count() == before) {
            auto text = projection.write(root, target, factir_enabled, moonstone);
            if (diagnostics.error_count() == before)
                return text;
        }
    } catch (const std::length_error &error) {
        unit.error(at, error.what(), "cephyr.limit");
    } catch (const std::logic_error &error) {
        if (diagnostics.error_count() == before)
            unit.error(at, error.what(), "cephyr.frontend.internal");
    }
    return std::nullopt;
}

} // namespace cephyr
