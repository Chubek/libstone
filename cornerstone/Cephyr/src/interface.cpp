#include "interface.hpp"

#include "declarations.hpp"

#include <cephyr/preprocess.hpp>

#include <AbstractSyntaxTree.h>

#include <algorithm>
#include <bit>
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
            declarations.push_back({std::move(name), std::move(type), nullptr, {}, {}});
        } else {
            auto &alias = declarations[it->second].alias;
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
            declarations.push_back({name, {}, &symbol, {}, {}});
        auto &declaration = declarations[it->second];
        declaration.flags.push_back(flags);
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
            out << "(pointer (bits " << type.bits << ") ";
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
            out << ')';
            for (const auto &member : record->members) {
                out << "\n    (field ";
                optional_name(out, member.name);
                out << " (offset " << member.offset << ')';
                if (const auto &bits = member.bit_field) {
                    out << " (bit-field (width " << bits->width << ") (shift " << bits->shift
                        << ") (container-offset " << member.offset - bits->byte_in_container << "))";
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
                           std::string_view calling_convention) const
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
                if (std::any_of(decl.flags.begin(), decl.flags.end(),
                                [](unsigned flags) { return (flags & FS_NORETURN) != 0; }))
                    out << " noreturn";
                out << ')';
            } else {
                const auto requested = symbol->alignment_declarations.empty() ? 0 :
                    symbol->alignment_declarations.front().first;
                out << " (layout ";
                layout(out, type, requested);
                out << ')';
            }
        }
        out << " (type ";
        write_type(out, type);
        out << "))";
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

    std::string write(std::string_view triple, std::string_view abi) const
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "(interface 1\n  (target ";
        quoted(out, triple);
        out << ")\n  (data-model LP64)\n  (abi ";
        quoted(out, abi);
        out << ')';
        for (std::size_t i = 0; i < tags.size(); ++i)
            write_tag(out, i, tags[i]);
        const auto convention = abi == "SysVAMD64" ? "sysv" : abi == "AAPCS64" ? "aapcs64" : "lp64";
        for (const auto &decl : declarations)
            write_declaration(out, decl, convention);
        out << ")\n";
        return std::move(out).str();
    }
};

} // namespace

std::optional<std::string> project_interface(cparser::ASTNode *root, const Preprocessed &source,
                                             Diagnostics &diagnostics, bool hosted,
                                             std::string_view triple, std::string_view abi)
{
    const auto before = diagnostics.error_count();
    Unit unit(source, diagnostics, hosted);
    auto *at = root;
    try {
        Projection projection(unit);
        projection.resolve(root, at);
        if (diagnostics.error_count() == before)
            return projection.write(triple, abi);
    } catch (const std::length_error &error) {
        unit.error(at, error.what(), "cephyr.limit");
    } catch (const std::logic_error &error) {
        if (diagnostics.error_count() == before)
            unit.error(at, error.what(), "cephyr.frontend.internal");
    }
    return std::nullopt;
}

} // namespace cephyr
