#include <cephyr/lower.hpp>

#include "declarations.hpp"

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
        (source.element->is_volatile && !destination.element->is_volatile) ||
        (source.element->is_restrict && !destination.element->is_restrict))
        return false;
    if (destination.element->kind == CType::Kind::void_ ||
        source.element->kind == CType::Kind::void_)
        return true;
    if (destination.element->is_atomic != source.element->is_atomic)
        return false;
    auto destination_element = *destination.element;
    auto source_element = *source.element;
    destination_element.clear_qualifiers();
    source_element.clear_qualifiers();
    return compatible(destination_element, source_element);
}

std::optional<Relocation> symbol_relocation(const TypeContext &context, const CType &destination,
                                            Node *expression, bool materialize) {
    auto &unit = context.unit;
    const auto *scope = context.function;
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
    const auto array_element = [&](Storage object, Node *index_node, bool allow_one_past,
                                   bool subtract = false) -> std::optional<Storage> {
        if (object.type.kind != CType::Kind::array || !object.type.element)
            return {};
        const auto index = integer_constant(index_node, context);
        const auto signed_index = index ? signed_value(*index) : std::nullopt;
        if (!signed_index || (subtract ? *signed_index > 0 : *signed_index < 0))
            return {};
        const auto position = subtract ? std::uint64_t{0} - static_cast<std::uint64_t>(*signed_index)
                                       : static_cast<std::uint64_t>(*signed_index);
        const auto count = object.type.count;
        auto element = *object.type.element;
        const auto stride = size_of(element);
        if ((count && (position > count || (!allow_one_past && position == count))) ||
            !stride || !*stride || position > (max_offset - object.offset) / *stride)
            return {};
        element.is_const |= object.type.is_const;
        element.is_volatile |= object.type.is_volatile;
        element.is_restrict |= object.type.is_restrict;
        object.type = std::move(element);
        object.offset += static_cast<std::int64_t>(position * *stride);
        return object;
    };
    // A block-scope object may shadow a file-scope symbol, but only one
    // with static duration is usable as an address constant (C11 6.6p9).
    const auto find_storage = [&](Node *id) -> std::optional<Storage> {
        const auto source_name = identifier(id);
        for (auto *prototype = context.prototype; prototype; prototype = prototype->parent)
            if (prototype->parameters.contains(source_name))
                return std::nullopt;
        if (scope) {
            if (const auto binding = scope->find_linked(source_name, id)) {
                auto &target = *binding->linked;
                if (materialize && target.internal && target.type.kind == CType::Kind::function)
                    target.used = true;
                return Storage{binding->type, target.name};
            }
            if (auto local = scope->find_local(source_name)) {
                if (local->static_name.empty())
                    return std::nullopt;
                if (materialize)
                    scope->require_static(*local);
                return Storage{local->type, local->static_name};
            }
            return std::nullopt;
        }
        auto *target = unit.find_file_symbol(source_name);
        if (!target)
            return std::nullopt;
        if (materialize && target->internal && target->type.kind == CType::Kind::function)
            target->used = true; // Diagnose an internal declaration without a definition.
        return Storage{target->type, target->name};
    };

    const auto subobject = [&](auto &&self, Node *node, bool allow_one_past) -> std::optional<Storage> {
        const AnalysisDepth guard(depth);
        if (present(node) && node->getKind() == cparser::NK_GENERIC_EXPR)
            node = select_generic(node, context);
        if (!present(node))
            return {};
        if (node->getKind() == cparser::NK_IDENT_NODE)
            return find_storage(node);
        if (node->getKind() == cparser::NK_STRING_CONST) {
            auto type = string_constant_type(node, unit.source);
            return Storage{type, materialize ? unit.add_string(node, type) : std::string{}};
        }
        if (node->getKind() == cparser::NK_COMPOUND_LITERAL) {
            if (scope) // A literal occurring within a function is automatic.
                return {};
            if (!materialize) {
                auto type = expression_type(node, context);
                return type.valid() ? std::optional<Storage>{Storage{std::move(type), {}}}
                                    : std::nullopt;
            }
            if (auto object = unit.add_static_compound_literal(node))
                return Storage{std::move(object->first), std::move(object->second)};
            return {};
        }
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
            return object ? array_element(std::move(*object), index_node, allow_one_past)
                          : std::nullopt;
        } else {
            return {};
        }
        if (!object)
            return {};
        const auto member = find_member(object->type, identifier(member_name));
        if (!member || member->offset > max_offset - object->offset)
            return {};
        if (member->bit_field) {
            if (address_of)
                context.diagnostics.error("cephyr.type", "cannot take the address of a bit-field",
                                          location(context.source, member_name));
            return {};
        }
        object->offset += static_cast<std::int64_t>(member->offset);
        object->type = member->type;
        return object;
    };

    // Support a single ICE addend from a literal's decay, as well as the
    // equivalent &literal[i] form handled in subobject.
    Node *literal = nullptr, *index = nullptr;
    bool subtract = false;
    const auto array_literal = [](Node *node) {
        return node && (node->getKind() == cparser::NK_STRING_CONST ||
                        node->getKind() == cparser::NK_COMPOUND_LITERAL);
    };
    if (!address_of && expression->getKind() == cparser::NK_PLUS_EXPR) {
        auto *plus = static_cast<cparser::PlusExprASTNode *>(expression);
        literal = plus->getLhs();
        index = plus->getRhs();
        if (!array_literal(literal))
            std::swap(literal, index);
    } else if (!address_of && expression->getKind() == cparser::NK_MINUS_EXPR) {
        auto *minus = static_cast<cparser::MinusExprASTNode *>(expression);
        if (!minus->isUnary()) {
            literal = minus->getLhs();
            index = minus->getRhs();
            subtract = true;
        }
    }
    const bool has_addend = array_literal(literal);
    auto storage = subobject(subobject, has_addend ? literal : expression, address_of);
    if (storage && has_addend) {
        storage = array_element(std::move(*storage), index, true, subtract);
        address_of = true; // The resulting relocation points at the selected element.
    }
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
        element.is_restrict |= type.is_restrict;
        source_type = pointer_type(element);
    } else {
        return {};
    }
    if (!pointer_compatible(destination, source_type))
        return {};
    return Relocation{std::move(storage->name), storage->offset};
}

std::optional<Constant> static_arithmetic_initializer(Unit &unit, const CType &type,
                                                     Node *initializer, const Function *scope) {
    bool valid = false;
    initializer = scalar_initializer(initializer, valid);
    if (!valid) {
        unit.error(initializer, "scalar initializer has more than one value",
                   "cephyr.global.initializer");
        return {};
    }
    const auto errors = unit.diagnostics.error_count();
    auto value = arithmetic_initializer_constant(initializer, type, TypeContext(unit, scope));
    if (!value) {
        if (errors == unit.diagnostics.error_count())
            unit.error(
                initializer,
                 "static initializer is not a supported arithmetic constant expression",
                 "cephyr.global.initializer");
    }
    return value;
}

bool initialize_arithmetic(Unit &unit, const CType &type, Node *initializer,
                            Image &image, std::uint64_t offset, const Function *scope,
                            std::optional<BitField> bit_field = std::nullopt) {
    const auto value = static_arithmetic_initializer(unit, type, initializer, scope);
    if (!value)
        return false;
    if (bit_field) {
        const auto field = *bit_field;
        const auto mask = field.mask() << field.shift;
        const auto bits = (value->value & field.mask()) << field.shift;
        for (unsigned i = 0; i < field.bytes(); ++i) {
            auto &byte = image.bytes[static_cast<std::size_t>(offset + i)];
            const auto byte_mask = (mask >> (i * 8)) & 0xff;
            byte = static_cast<unsigned char>((static_cast<unsigned char>(byte) & ~byte_mask) |
                   (bits >> (i * 8)));
        }
        return true;
    }
    const auto width = size_of(type);
    if (type.extended_real()) {
        // SysV AMD64 stores the 10-byte x87 payload at the beginning of a
        // 16-byte object. The zero-filled image supplies its trailing padding.
        write_integer(image, offset, value->value, 8);
        write_integer(image, offset + 8, value->high_word, type.bits == 128 ? 8 : 2);
        return true;
    }
    if (!width || *width > 8 || !type.bits || type.bits > 64) {
        unit.error(initializer, "arithmetic object width is not supported",
                   "cephyr.global.type");
        return false;
    }
    write_integer(image, offset, value->value, *width);
    return true;
}

bool initialize_pointer(const TypeContext &context, const CType &type, Node *initializer,
                        Image *image, std::uint64_t offset = 0) {
    const auto error = [&](std::string_view code, std::string_view message) {
        context.diagnostics.error(code, message, location(context.source, initializer));
    };
    bool valid = false;
    initializer = scalar_initializer(initializer, valid);
    if (!valid) {
        error("cephyr.global.initializer", "scalar initializer has more than one value");
        return false;
    }
    if (initializer->getKind() == cparser::NK_GENERIC_EXPR) {
        initializer = select_generic(initializer, context);
        if (!initializer)
            return false;
    }

    const auto before_relocation = context.diagnostics.error_count();
    if (auto relocation = symbol_relocation(context, type, initializer, image != nullptr)) {
        if (image)
            image->relocations.emplace(offset, std::move(*relocation));
        return true;
    }
    if (context.diagnostics.error_count() != before_relocation)
        return false;

    const auto errors = context.diagnostics.error_count();
    Node *integer_node = initializer;
    if (initializer->getKind() == cparser::NK_CAST_EXPR) {
        auto *cast = static_cast<cparser::CastExprASTNode *>(initializer);
        auto cast_type = read_type(cast->getType(), context, true);
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
        auto value = integer_constant(integer_node, context);
        if (value && normalize(value->value, value->type) == 0)
            return true;
    }

    if (errors == context.diagnostics.error_count())
        error("cephyr.global.relocation",
              "pointer initializer is neither a null pointer constant nor a supported symbol address");
    return false;
}

bool initialize_scalar(Unit &unit, const CType &type, Node *initializer,
                          Image &image, std::uint64_t offset, const Function *scope) {
    if (type.arithmetic())
        return initialize_arithmetic(unit, type, initializer, image, offset, scope);
    if (type.pointer())
        return initialize_pointer(TypeContext(unit, scope), type, initializer, &image, offset);
    unit.error(initializer, "static structure or union initializers must use braces",
               "cephyr.global.type");
    return false;
}

bool initialize_aggregate(Unit &unit, const std::vector<ObjectInitEntry> &plan, Image &image,
                          const Function *scope) {
    for (const auto &entry : plan) {
        if (entry.bit_field) {
            // Union member changes are handled by the planner's separate
            // reset entries. Within a byte preserve all neighboring fields.
            if (!initialize_arithmetic(unit, entry.type, entry.source, image, entry.offset,
                                       scope, entry.bit_field))
                return false;
            continue;
        }
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
        if (type.record->is_union) {
            const auto first = std::find_if_not(members.begin(), members.end(),
                [](const RecordMember &member) { return member.unnamed_bit_field(); });
            return first != members.end() && zero_representable(first->type);
        }
        return std::all_of(members.begin(), members.end(),
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

bool validate_static_compound_initializer(const TypeContext &context,
                                          const std::vector<ObjectInitEntry> &plan) {
    for (const auto &entry : plan) {
        if (entry.clear || entry.bytes)
            continue;
        if (entry.type.pointer()) {
            if (!initialize_pointer(context, entry.type, entry.source, nullptr))
                return false;
        } else {
            bool valid;
            auto *value = scalar_initializer(entry.source, valid);
            const auto errors = context.diagnostics.error_count();
            if (valid && entry.type.arithmetic() &&
                arithmetic_initializer_constant(value, entry.type, context))
                continue;
            if (errors == context.diagnostics.error_count())
                context.diagnostics.error("cephyr.compound-literal",
                    "static compound literal initializer is not a supported arithmetic constant expression",
                    location(context.source, value));
            return false;
        }
    }
    return true;
}

std::string Unit::add_string(Node *literal, const CType &type) {
    // Do not share a byte-identical object with an insufficient alignment.
    auto key = std::pair{align_of(type), string_constant_bytes(literal)};
    if (const auto found = strings.find(key); found != strings.end())
        return found->second;

    auto name = generated_name(*this, "string");

    Image image;
    image.bytes.assign(key.second.begin(), key.second.end());
    image.size = image.bytes.size();

    globals.push_back(Global{name, 0, {}});
    data_assembly += object_assembly(name, true, true, key.first, image);
    strings.emplace(std::move(key), name);
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

namespace {

bool define_global_object(Unit &unit, Symbol &symbol, Node *initializer,
                          const Function *scope,
                          std::optional<std::vector<ObjectInitEntry>> aggregate_init = {}) {
    Node *definition = symbol.definition;
    if (!valid_symbol(symbol.name)) {
        unit.error(definition, "global assembler name is invalid",
                   "cephyr.global.symbol");
        return false;
    }
    if (!global_name_available(unit, symbol.name, definition))
        return false;

    // An inferred bound completes the visible type only at the end of its
    // initializer list (C11 6.7.9p22), not during initializer evaluation.
    CType type = symbol.type;
    if (!aggregate_init && type.aggregate() && present(initializer)) {
        aggregate_init = plan_object_initializer(TypeContext(unit, scope), type, initializer);
        if (!aggregate_init)
            return false;
    }
    if (type.kind == CType::Kind::array && !type.count && !present(initializer)) {
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
    const auto alignment = std::max(align_of(type), symbol.definition_alignment.value_or(0));
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

} // namespace

bool validate_interface_initializer(Unit &unit, Symbol &symbol) {
    const TypeContext context(unit);
    auto *declaration = static_cast<cparser::VarDeclASTNode *>(symbol.definition);
    auto *initializer = declaration->getInit();
    auto type = symbol.type;
    std::optional<std::vector<ObjectInitEntry>> plan;
    if (type.aggregate()) {
        plan = plan_object_initializer(context, type, initializer);
        if (!plan)
            return false;
    }
    if (!size_of(type)) {
        unit.error(declaration, "definition requires a complete object type", "cephyr.global.layout");
        return false;
    }
    // Share the static-initializer rules without allocating an object image,
    // producing relocations, or imposing native image-size limits.
    const auto check = [&](const CType &destination, Node *value) {
        if (destination.arithmetic())
            return static_arithmetic_initializer(unit, destination, value, nullptr).has_value();
        if (destination.pointer())
            return initialize_pointer(context, destination, value, nullptr);
        unit.error(value, "static structure or union initializers must use braces", "cephyr.global.type");
        return false;
    };
    if (plan) {
        for (const auto &entry : *plan)
            if (!entry.clear && !entry.bytes && !check(entry.type, entry.source))
                return false;
    } else if (!check(type, initializer)) {
        return false;
    }
    symbol.type = std::move(type);
    return true;
}

bool define_global(Unit &unit, Symbol &symbol, const Function *scope) {
    auto *definition = symbol.definition;
    if (!present(definition) || definition->getKind() != cparser::NK_VAR_DECL) {
        unit.error(definition, "global definition is not a variable declaration",
                   "cephyr.global.definition");
        return false;
    }
    auto *declaration = static_cast<cparser::VarDeclASTNode *>(definition);
    return define_global_object(unit, symbol, declaration->getInit(), scope);
}

std::optional<std::pair<CType, std::string>> Unit::add_static_compound_literal(Node *node) {
    if (const auto found = static_compound_literals.find(node); found != static_compound_literals.end())
        return found->second;
    CType type;
    auto plan = plan_compound_literal(TypeContext(*this), node, type);
    if (!plan)
        return std::nullopt;
    // The TU prefix is reserved, and nested initializers can themselves
    // materialize literals before this one is emitted.
    auto name = "__cephyr_tu_compound_" + std::to_string(compound_literals++);
    Symbol storage{type, std::move(name), node, node, true, false, true};
    auto *initializer = static_cast<cparser::CompoundLiteralASTNode *>(node)->getInit();
    if (!define_global_object(*this, storage, initializer, nullptr, std::move(plan)))
        return std::nullopt;
    return static_compound_literals.emplace(node, std::pair{type, storage.name}).first->second;
}

} // namespace cephyr::detail
