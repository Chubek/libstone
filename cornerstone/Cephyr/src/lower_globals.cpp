#include <cephyr/lower.hpp>

#include "floating_literal.hpp"

#include <AbstractSyntaxTree.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cephyr::detail {
namespace {

// Keep source-controlled assembly reasonably small.  Objects with no explicit
// initializer do not need an in-memory image and are subject to the larger
// object limit instead.
constexpr std::uint64_t max_initialized_bytes = 4u * 1024u * 1024u;
constexpr std::uint64_t max_object_bytes = 64u * 1024u * 1024u;
constexpr std::size_t max_native_words = 65536;

struct Relocation {
    std::string symbol;
    std::int64_t addend = 0;
};

struct Image {
    std::uint64_t size = 0;
    bool implicit_zero = false;
    std::vector<std::uint8_t> bytes;
    std::map<std::uint64_t, Relocation> relocations;
};

bool symbol_head(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           value == '_';
}

bool valid_symbol(std::string_view name) {
    if (name.empty() || name.size() > 255 ||
        !symbol_head(static_cast<unsigned char>(name.front())))
        return false;
    return std::all_of(name.begin() + 1, name.end(), [](char character) {
        const auto value = static_cast<unsigned char>(character);
        return symbol_head(value) || (value >= '0' && value <= '9');
    });
}

bool name_in_use(const Unit &unit, std::string_view name) {
    for (const auto &[source_name, symbol] : unit.symbols) {
        (void)source_name;
        if (symbol.name == name)
            return true;
    }
    for (const auto &global : unit.globals)
        if (global.name == name)
            return true;
    for (const auto &[bytes, string_name] : unit.strings) {
        (void)bytes;
        if (string_name == name)
            return true;
    }
    return false;
}

std::string generated_name(Unit &unit, std::string_view stem) {
    for (std::uint64_t index = unit.strings.size();; ++index) {
        auto candidate = std::string("__cephyr_tu_") + std::string(stem) + "_" +
                         std::to_string(index);
        if (!name_in_use(unit, candidate))
            return candidate;
    }
}

bool read_only(const CType &type) {
    return type.is_const ||
           (type.kind == CType::Kind::array && type.element && read_only(*type.element));
}

bool all_zero(const std::vector<std::uint8_t> &bytes) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::uint8_t value) { return value == 0; });
}

bool zero_image(const Image &image) {
    return image.implicit_zero ||
           (image.relocations.empty() && all_zero(image.bytes));
}

void append_byte_directives(std::string &out, const std::vector<std::uint8_t> &bytes,
                            std::uint64_t begin, std::uint64_t end) {
    static constexpr char hex[] = "0123456789abcdef";
    while (begin < end) {
        std::uint64_t zero_end = begin;
        while (zero_end < end && bytes[static_cast<std::size_t>(zero_end)] == 0)
            ++zero_end;
        if (zero_end - begin >= 8) {
            out += "\t.zero " + std::to_string(zero_end - begin) + "\n";
            begin = zero_end;
            continue;
        }

        const auto line_end = std::min<std::uint64_t>(end, begin + 16);
        out += "\t.byte ";
        for (auto offset = begin; offset < line_end; ++offset) {
            if (offset != begin)
                out.push_back(',');
            const auto value = bytes[static_cast<std::size_t>(offset)];
            out += "0x";
            out.push_back(hex[value >> 4]);
            out.push_back(hex[value & 0x0f]);
        }
        out.push_back('\n');
        begin = line_end;
    }
}

void append_image(std::string &out, const Image &image, bool zero) {
    if (zero) {
        out += "\t.zero " + std::to_string(image.size) + "\n";
        return;
    }

    std::uint64_t offset = 0;
    for (const auto &[position, relocation] : image.relocations) {
        append_byte_directives(out, image.bytes, offset, position);
        out += "\t.quad " + relocation.symbol;
        if (relocation.addend > 0)
            out += "+" + std::to_string(relocation.addend);
        else if (relocation.addend < 0)
            out += std::to_string(relocation.addend);
        out.push_back('\n');
        offset = position + 8;
    }
    append_byte_directives(out, image.bytes, offset, image.size);
}

std::string object_assembly(std::string_view name, bool internal, bool readonly,
                            unsigned alignment, const Image &image) {
    const bool zero = zero_image(image);
    std::string out;
    if (readonly) {
        // Read-only objects requiring link/load-time fixups belong in RELRO,
        // not .rodata: PIE/shared links must not create text relocations.
        out += image.relocations.empty() ? "\t.section .rodata\n"
                                         : "\t.section .data.rel.ro\n";
    } else if (zero) {
        out += "\t.section .bss\n";
    } else {
        out += "\t.section .data\n";
    }
    out += "\t.balign " + std::to_string(alignment) + "\n";
    out += internal ? "\t.local " : "\t.globl ";
    out.append(name);
    out += "\n\t.type ";
    out.append(name);
    out += ", %object\n";
    out.append(name);
    out += ":\n";
    append_image(out, image, zero);
    out += "\t.size ";
    out.append(name);
    out += ", .-";
    out.append(name);
    out.push_back('\n');
    return out;
}

Node *scalar_initializer(Node *initializer, bool &valid) {
    valid = true;
    while (present(initializer) && initializer->getKind() == cparser::NK_LIST) {
        auto values = elements(initializer);
        if (values.size() != 1) {
            valid = false;
            return initializer;
        }
        initializer = values.front();
    }
    return initializer;
}

void write_integer(Image &image, std::uint64_t offset, std::uint64_t value,
                   std::uint64_t width) {
    for (std::uint64_t byte = 0; byte < width; ++byte)
        image.bytes[static_cast<std::size_t>(offset + byte)] =
            static_cast<std::uint8_t>(value >> (8 * byte));
}

std::optional<std::int64_t> signed_value(const Constant &constant) {
    if (!constant.type.integer() || !constant.type.bits || constant.type.bits > 64)
        return std::nullopt;
    auto value = normalize(constant.value, constant.type);
    if (constant.type.is_signed && constant.type.bits < 64) {
        const auto sign = std::uint64_t{1} << (constant.type.bits - 1);
        if (value & sign)
            value |= ~((std::uint64_t{1} << constant.type.bits) - 1);
        return std::bit_cast<std::int64_t>(value);
    }
    if (!constant.type.is_signed &&
        value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return std::nullopt;
    return std::bit_cast<std::int64_t>(value);
}

bool pointer_compatible(const CType &destination, const CType &source) {
    if (!destination.pointer() || !source.pointer() || !destination.element ||
        !source.element)
        return false;
    if (destination.function_pointer() != source.function_pointer())
        return false;
    // Initialization may add, but not discard, pointed-to qualifiers.
    if ((source.element->is_const && !destination.element->is_const) ||
        (source.element->is_volatile && !destination.element->is_volatile))
        return false;
    if (destination.element->kind == CType::Kind::void_ ||
        source.element->kind == CType::Kind::void_)
        return true;
    auto destination_element = *destination.element;
    auto source_element = *source.element;
    destination_element.is_const = source_element.is_const = false;
    destination_element.is_volatile = source_element.is_volatile = false;
    return compatible(destination_element, source_element);
}

bool string_pointer_compatible(const CType &destination, const Preprocessed &source) {
    return pointer_compatible(destination, pointer_type(plain_char_type(source)));
}

std::optional<Relocation> symbol_relocation(Unit &unit, const CType &destination,
                                            Node *expression, const Function *scope) {
    bool address_of = false;
    if (expression->getKind() == cparser::NK_ADDR_EXPR) {
        address_of = true;
        expression =
            static_cast<cparser::AddrExprASTNode *>(expression)->getExpr();
    }

    struct Storage {
        CType type;
        std::string name;
        std::int64_t offset = 0;
    };
    constexpr auto max_offset = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    unsigned depth = 0;
    // A block-scope object may shadow a file-scope symbol, but only one
    // with static duration is usable as an address constant (C11 6.6p9).
    const auto find_storage = [&](Node *id) -> std::optional<Storage> {
        const auto source_name = identifier(id);
        if (scope) {
            if (auto local = scope->find_local(source_name)) {
                if (local->static_name.empty())
                    return std::nullopt;
                scope->require_static(*local);
                return Storage{local->type, local->static_name};
            }
        }
        const auto found = unit.symbols.find(source_name);
        if (found == unit.symbols.end())
            return std::nullopt;
        auto &target = found->second;
        if (target.internal && target.type.kind == CType::Kind::function)
            target.used = true; // Diagnose an internal declaration without a definition.
        return Storage{target.type, target.name};
    };

    const auto subobject = [&](auto &&self, Node *node, bool allow_one_past) -> std::optional<Storage> {
        const AnalysisDepth guard(depth);
        if (present(node) && node->getKind() == cparser::NK_GENERIC_EXPR)
            node = select_generic(node, TypeContext(unit, scope));
        if (!present(node))
            return {};
        if (node->getKind() == cparser::NK_IDENT_NODE)
            return find_storage(node);
        std::optional<Storage> object;
        Node *member_name = nullptr;
        if (node->getKind() == cparser::NK_STRUCT_REF) {
            auto *dot = static_cast<cparser::StructRefASTNode *>(node);
            object = self(self, dot->getName(), false);
            member_name = dot->getMember();
        } else if (node->getKind() == cparser::NK_INDIRECT_REF) {
            auto *arrow = static_cast<cparser::IndirectRefASTNode *>(node);
            if (!present(arrow->getField()) || arrow->getExpr()->getKind() != cparser::NK_ADDR_EXPR)
                return {};
            auto *address = static_cast<cparser::AddrExprASTNode *>(arrow->getExpr());
            object = self(self, address->getExpr(), false);
            member_name = arrow->getField();
        } else if (node->getKind() == cparser::NK_ARRAY_REF) {
            auto *ref = static_cast<cparser::ArrayRefASTNode *>(node);
            object = self(self, ref->getExpr(), false);
            auto *index_node = ref->getIndex();
            if (!object) {
                object = self(self, index_node, false); // i[a] == a[i]
                index_node = ref->getExpr();
            }
            if (!object || object->type.kind != CType::Kind::array || !object->type.element)
                return {};
            const auto index = integer_constant(index_node, TypeContext(unit, scope));
            const auto signed_index = index ? signed_value(*index) : std::nullopt;
            if (!signed_index || *signed_index < 0)
                return {};
            const auto position = static_cast<std::uint64_t>(*signed_index);
            const auto count = object->type.count;
            auto element = *object->type.element;
            const auto stride = size_of(element);
            if ((count && (position > count || (!allow_one_past && position == count))) ||
                !stride || !*stride || position > (max_offset - object->offset) / *stride)
                return {};
            element.is_const |= object->type.is_const;
            element.is_volatile |= object->type.is_volatile;
            object->type = std::move(element);
            object->offset += static_cast<std::int64_t>(position * *stride);
            return object;
        } else {
            return {};
        }
        if (!object)
            return {};
        const auto *member = find_member(object->type, identifier(member_name));
        if (!member || member->offset > max_offset - object->offset)
            return {};
        object->offset += static_cast<std::int64_t>(member->offset);
        object->type = qualified_member_type(object->type, *member);
        return object;
    };

    auto storage = subobject(subobject, expression, address_of);
    if (!storage)
        return {};
    auto &type = storage->type;
    CType source_type;
    if (address_of || type.kind == CType::Kind::function) {
        source_type = pointer_type(type);
    } else if (type.kind == CType::Kind::array && type.element) {
        auto element = *type.element;
        element.is_const |= type.is_const;
        element.is_volatile |= type.is_volatile;
        source_type = pointer_type(element);
    } else {
        return {};
    }
    if (!pointer_compatible(destination, source_type))
        return {};
    return Relocation{std::move(storage->name), storage->offset};
}

bool initialize_integer(Unit &unit, const CType &type, Node *initializer,
                        Image &image, std::uint64_t offset, const Function *scope) {
    bool valid = false;
    initializer = scalar_initializer(initializer, valid);
    if (!valid) {
        unit.error(initializer, "scalar initializer has more than one value",
                   "cephyr.global.initializer");
        return false;
    }
    const auto errors = unit.diagnostics.error_count();
    auto value = integer_constant(initializer, TypeContext(unit, scope));
    if (!value) {
        if (errors == unit.diagnostics.error_count())
            unit.error(
                initializer,
                "file-scope integer initializer is not an integer constant expression",
                "cephyr.global.initializer");
        return false;
    }
    const auto width = size_of(type);
    if (!width || *width > 8 || !type.bits || type.bits > 64) {
        unit.error(initializer, "integer object width is not supported",
                   "cephyr.global.type");
        return false;
    }
    write_integer(image, offset, normalize(value->value, type), *width);
    return true;
}

bool initialize_pointer(Unit &unit, const CType &type, Node *initializer,
                         Image &image, std::uint64_t offset, const Function *scope) {
    bool valid = false;
    initializer = scalar_initializer(initializer, valid);
    if (!valid) {
        unit.error(initializer, "scalar initializer has more than one value",
                   "cephyr.global.initializer");
        return false;
    }
    if (initializer->getKind() == cparser::NK_GENERIC_EXPR) {
        initializer = select_generic(initializer, TypeContext(unit, scope));
        if (!initializer)
            return false;
    }

    if (initializer->getKind() == cparser::NK_STRING_CONST &&
        string_pointer_compatible(type, unit.source)) {
        const auto errors = unit.diagnostics.error_count();
        auto bytes = string_constant(initializer, unit.source, unit.diagnostics);
        if (!bytes && errors == unit.diagnostics.error_count())
            unit.error(initializer, "invalid string literal in pointer initializer",
                       "cephyr.global.string");
        if (!bytes)
            return false;
        image.relocations.emplace(offset, Relocation{unit.add_string(std::move(*bytes)), 0});
        return true;
    }

    const auto before_relocation = unit.diagnostics.error_count();
    if (auto relocation = symbol_relocation(unit, type, initializer, scope)) {
        image.relocations.emplace(offset, std::move(*relocation));
        return true;
    }
    if (unit.diagnostics.error_count() != before_relocation)
        return false;

    const auto errors = unit.diagnostics.error_count();
    Node *integer_node = initializer;
    if (initializer->getKind() == cparser::NK_CAST_EXPR) {
        auto *cast = static_cast<cparser::CastExprASTNode *>(initializer);
        auto cast_type = read_type(cast->getType(), TypeContext(unit, scope), true);
        if (cast_type.pointer()) {
            // A cast of an integer constant zero to void * is itself a
            // null-pointer constant, notably in the packaged NULL macro.
            const bool void_pointer =
                cast_type.element && cast_type.element->kind == CType::Kind::void_;
            integer_node = void_pointer || pointer_compatible(type, cast_type)
                               ? cast->getExpr()
                               : nullptr;
        }
    }
    if (present(integer_node)) {
        auto value = integer_constant(integer_node, TypeContext(unit, scope));
        if (value && normalize(value->value, value->type) == 0)
            return true;
    }

    if (errors == unit.diagnostics.error_count())
        unit.error(initializer,
                   "pointer initializer is neither a null pointer constant nor a supported "
                   "symbol address",
                   "cephyr.global.relocation");
    return false;
}

bool initialize_real(Unit &unit, const CType &type, Node *initializer,
                     Image &image, std::uint64_t offset, const Function *scope) {
    bool valid = false;
    initializer = scalar_initializer(initializer, valid);
    if (!valid) {
        unit.error(initializer, "scalar initializer has more than one value",
                   "cephyr.global.initializer");
        return false;
    }
    const TypeContext context(unit, scope);
    Node *leaf = initializer;
    bool negative = false;
    while (present(leaf)) {
        if (leaf->getKind() == cparser::NK_UNARY_PLUS_EXPR) {
            leaf = static_cast<cparser::UnaryPlusASTNode *>(leaf)->getExpr();
        } else if (leaf->getKind() == cparser::NK_MINUS_EXPR) {
            auto *minus = static_cast<cparser::MinusExprASTNode *>(leaf);
            if (!minus->isUnary())
                break;
            negative = !negative;
            leaf = minus->getRhs();
        } else {
            break;
        }
    }

    FloatingLiteral converted;
    if (present(leaf) && leaf->getKind() == cparser::NK_REAL_CONST) {
        const auto value = real_constant(leaf, context);
        if (!value)
            return false;
        const auto sign = std::uint64_t(negative) << (value->type.bits - 1);
        converted = convert_floating_literal(
            {FloatingLiteralStatus::ok, value->type.bits, value->value ^ sign}, type.bits);
    } else {
        const auto errors = unit.diagnostics.error_count();
        const auto value = integer_constant(initializer, context);
        if (!value) {
            if (errors == unit.diagnostics.error_count())
                unit.error(initializer,
                    "static floating-point initializer requires a signed floating literal "
                    "or an integer constant expression", "cephyr.global.initializer");
            return false;
        }
        converted = integer_to_floating_literal(normalize(value->value, value->type),
                                                 value->type.is_signed, type.bits);
    }
    if (!converted) {
        unit.error(initializer, "static initializer is out of range for the floating-point object",
                   "cephyr.global.initializer");
        return false;
    }
    write_integer(image, offset, converted.bits, type.bits / 8);
    return true;
}

bool initialize_scalar(Unit &unit, const CType &type, Node *initializer,
                         Image &image, std::uint64_t offset, const Function *scope) {
    if (type.integer())
        return initialize_integer(unit, type, initializer, image, offset, scope);
    if (type.real())
        return initialize_real(unit, type, initializer, image, offset, scope);
    if (type.pointer())
        return initialize_pointer(unit, type, initializer, image, offset, scope);
    unit.error(initializer, "static structure or union initializers must use braces",
               "cephyr.global.type");
    return false;
}

bool initialize_aggregate(Unit &unit, const std::vector<ObjectInitEntry> &plan, Image &image,
                          const Function *scope) {
    for (const auto &entry : plan) {
        const auto extent = entry.bytes ? entry.bytes->size() : *size_of(entry.type);
        const auto end = entry.offset + extent;
        // A later designator may override a pointer, or switch the active
        // member of a union containing a pointer. Remove its previous fixup
        // as well as its bytes. Non-overlapping fixups remain unchanged.
        image.relocations.erase(image.relocations.lower_bound(entry.offset > 7 ? entry.offset - 7 : 0),
                                image.relocations.lower_bound(end));
        std::fill(image.bytes.begin() + static_cast<std::size_t>(entry.offset),
                  image.bytes.begin() + static_cast<std::size_t>(end), 0);
        if (entry.clear)
            continue;
        if (entry.bytes) {
            std::copy(entry.bytes->begin(), entry.bytes->end(),
                      image.bytes.begin() + static_cast<std::size_t>(entry.offset));
        } else if (!initialize_scalar(unit, entry.type, entry.source, image, entry.offset,
                                      scope)) {
            return false;
        }
    }
    return true;
}

bool zero_representable(const CType &type) {
    if (type.scalar())
        return true;
    if (type.kind == CType::Kind::array)
        return type.count && type.element && zero_representable(*type.element);
    if (type.kind == CType::Kind::record && type.record && type.record->size) {
        const auto &members = type.record->members;
        return std::all_of(members.begin(), type.record->is_union ? members.begin() + 1 : members.end(),
                           [](const RecordMember &member) { return zero_representable(member.type); });
    }
    return false;
}

std::vector<std::uint64_t> words(const Image &image) {
    if (zero_image(image))
        return {};
    std::vector<std::uint64_t> result(
        static_cast<std::size_t>(image.size / 8), 0);
    for (std::uint64_t offset = 0; offset < image.size; ++offset)
        result[static_cast<std::size_t>(offset / 8)] |=
            std::uint64_t{image.bytes[static_cast<std::size_t>(offset)]}
            << (8 * (offset % 8));
    return result;
}

std::size_t native_words_in(const Unit &unit) {
    std::size_t total = 0;
    for (const auto &global : unit.globals) {
        if (global.words > max_native_words - std::min(total, max_native_words))
            return max_native_words;
        total += global.words;
    }
    return total;
}

bool global_name_available(Unit &unit, std::string_view name, Node *at) {
    const auto duplicate =
        std::find_if(unit.globals.begin(), unit.globals.end(),
                     [&](const Global &global) { return global.name == name; });
    if (duplicate == unit.globals.end())
        return true;
    unit.error(at, "duplicate global storage record", "cephyr.global.duplicate");
    return false;
}

} // namespace

std::string Unit::add_string(std::string bytes) {
    // The semantic string value already includes the implicit terminator.
    if (const auto found = strings.find(bytes); found != strings.end())
        return found->second;

    auto name = generated_name(*this, "string");

    Image image;
    image.bytes.assign(bytes.begin(), bytes.end());
    image.size = image.bytes.size();

    globals.push_back(Global{name, 0, {}});
    data_assembly += object_assembly(name, true, true, 1, image);
    strings.emplace(std::move(bytes), name);
    return name;
}

void Unit::require_function_name(const std::string &symbol, std::string_view spelling) {
    if (std::any_of(globals.begin(), globals.end(),
                    [&](const Global &global) { return global.name == symbol; }))
        return;
    Image image;
    image.bytes.assign(spelling.begin(), spelling.end());
    image.bytes.push_back(0);
    image.size = image.bytes.size();
    globals.push_back(Global{symbol, 0, {}});
    data_assembly += object_assembly(symbol, true, true, 1, image);
}

bool define_global(Unit &unit, Symbol &symbol, const Function *scope) {
    Node *definition = symbol.definition;
    if (!present(definition) ||
        definition->getKind() != cparser::NK_VAR_DECL) {
        unit.error(definition, "global definition is not a variable declaration",
                   "cephyr.global.definition");
        return false;
    }
    if (!valid_symbol(symbol.name)) {
        unit.error(definition, "global assembler name is invalid",
                   "cephyr.global.symbol");
        return false;
    }
    if (!global_name_available(unit, symbol.name, definition))
        return false;

    auto *declaration =
        static_cast<cparser::VarDeclASTNode *>(definition);
    auto *initializer = declaration->getInit();
    // An inferred bound completes the visible type only at the end of its
    // initializer list (C11 6.7.9p22), not during initializer evaluation.
    CType type = symbol.type;
    std::optional<std::vector<ObjectInitEntry>> aggregate_init;
    if (type.aggregate() && present(initializer)) {
        aggregate_init = plan_object_initializer(TypeContext(unit, scope), type, initializer);
        if (!aggregate_init)
            return false;
    } else if (type.kind == CType::Kind::array && !type.count) {
        if (symbol.internal) {
            unit.error(definition, scope ? "block-scope static array has incomplete type"
                                         : "tentative definition with internal linkage has incomplete array type",
                       "cephyr.global.incomplete-array");
            return false;
        }
        // C11 6.9.2: complete external tentative definitions at TU end.
        type.count = 1;
    }

    const auto size = size_of(type);
    const auto alignment = align_of(type);
    if (!size || !*size || *size > max_object_bytes || !alignment ||
        (alignment & (alignment - 1)) != 0) {
        unit.error(definition, "global object has an unsupported size or alignment",
                   "cephyr.global.layout");
        return false;
    }

    Image image;
    image.size = *size;

    if (!present(initializer)) {
        if (!zero_representable(type)) {
            unit.error(definition, "zero initialization of this object type is unsupported",
                       "cephyr.global.type");
            return false;
        }
        image.implicit_zero = true;
    } else {
        if (*size > max_initialized_bytes) {
            unit.error(initializer, "file-scope initializer exceeds the supported size limit",
                       "cephyr.global.initializer-limit");
            return false;
        }
        image.bytes.assign(static_cast<std::size_t>(*size), 0);
        if (type.scalar()) {
            if (!initialize_scalar(unit, type, initializer, image, 0, scope))
                return false;
        } else if (aggregate_init) {
            if (!initialize_aggregate(unit, *aggregate_init, image, scope))
                return false;
        } else {
            unit.error(initializer, "file-scope aggregate initializer is not supported",
                       "cephyr.global.aggregate");
            return false;
        }
    }

    const auto native_count = static_cast<std::size_t>(*size / 8);
    const bool readonly = read_only(type);
    const bool native =
        !readonly && image.relocations.empty() && *size % 8 == 0 &&
        alignment == 8 && native_count <= max_native_words - native_words_in(unit);
    if (native) {
        unit.globals.push_back(Global{symbol.name, native_count, words(image)});
        if (symbol.internal)
            unit.locals.push_back(symbol.name);
    } else {
        unit.globals.push_back(Global{symbol.name, 0, {}});
        unit.data_assembly +=
            object_assembly(symbol.name, symbol.internal, readonly, alignment, image);
    }
    symbol.type = std::move(type);
    return true;
}

} // namespace cephyr::detail
