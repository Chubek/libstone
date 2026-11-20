#include <cephyr/lower.hpp>

#include <AbstractSyntaxTree.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    std::uint64_t offset = 0;
    std::string symbol;
    std::int64_t addend = 0;
};

struct Image {
    std::uint64_t size = 0;
    bool implicit_zero = false;
    std::vector<std::uint8_t> bytes;
    std::vector<Relocation> relocations;
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
    for (const auto &relocation : image.relocations) {
        append_byte_directives(out, image.bytes, offset, relocation.offset);
        out += "\t.quad " + relocation.symbol;
        if (relocation.addend > 0)
            out += "+" + std::to_string(relocation.addend);
        else if (relocation.addend < 0)
            out += std::to_string(relocation.addend);
        out.push_back('\n');
        offset = relocation.offset + 8;
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
                                            Node *expression) {
    bool address_of = false;
    if (expression->getKind() == cparser::NK_ADDR_EXPR) {
        address_of = true;
        expression =
            static_cast<cparser::AddrExprASTNode *>(expression)->getExpr();
    }

    std::int64_t addend = 0;
    std::string source_name;
    CType source_type;

    if (expression->getKind() == cparser::NK_IDENT_NODE) {
        source_name = identifier(expression);
        const auto found = unit.symbols.find(source_name);
        if (found == unit.symbols.end())
            return std::nullopt;
        auto &target = found->second;
        if (!valid_symbol(target.name))
            return std::nullopt;

        if (address_of) {
            source_type = pointer_type(target.type);
        } else if (target.type.kind == CType::Kind::array && target.type.element) {
            source_type = pointer_type(*target.type.element);
        } else if (target.type.kind == CType::Kind::function) {
            source_type = pointer_type(target.type);
        } else {
            return std::nullopt;
        }
        if (!pointer_compatible(destination, source_type))
            return std::nullopt;
        if (target.internal && target.type.kind == CType::Kind::function)
            target.used = true; // Diagnose an internal declaration without a definition.
        return Relocation{0, target.name, 0};
    }

    if (!address_of || expression->getKind() != cparser::NK_ARRAY_REF)
        return std::nullopt;

    auto *reference = static_cast<cparser::ArrayRefASTNode *>(expression);
    auto *base = reference->getExpr();
    if (!present(base) || base->getKind() != cparser::NK_IDENT_NODE)
        return std::nullopt;
    source_name = identifier(base);
    const auto found = unit.symbols.find(source_name);
    if (found == unit.symbols.end() ||
        found->second.type.kind != CType::Kind::array ||
        !found->second.type.element || !valid_symbol(found->second.name))
        return std::nullopt;

    auto index =
        integer_constant(reference->getIndex(), unit.source, unit.diagnostics);
    auto signed_index = index ? signed_value(*index) : std::nullopt;
    const auto count = found->second.type.count;
    const auto element_size = size_of(*found->second.type.element);
    if (!signed_index || *signed_index < 0 ||
        (count && static_cast<std::uint64_t>(*signed_index) > count) ||
        !element_size || !*element_size ||
        static_cast<std::uint64_t>(*signed_index) >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
                *element_size)
        return std::nullopt;

    source_type = pointer_type(*found->second.type.element);
    if (!pointer_compatible(destination, source_type))
        return std::nullopt;
    addend = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(*signed_index) * *element_size);
    return Relocation{0, found->second.name, addend};
}

bool initialize_integer(Unit &unit, const CType &type, Node *initializer,
                        Image &image, std::uint64_t offset) {
    bool valid = false;
    initializer = scalar_initializer(initializer, valid);
    if (!valid) {
        unit.error(initializer, "scalar initializer has more than one value",
                   "cephyr.global.initializer");
        return false;
    }
    const auto errors = unit.diagnostics.error_count();
    auto value = integer_constant(initializer, unit.source, unit.diagnostics);
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
                        Image &image, std::uint64_t offset) {
    bool valid = false;
    initializer = scalar_initializer(initializer, valid);
    if (!valid) {
        unit.error(initializer, "scalar initializer has more than one value",
                   "cephyr.global.initializer");
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
        image.relocations.push_back(
            Relocation{offset, unit.add_string(std::move(*bytes)), 0});
        return true;
    }

    if (auto relocation = symbol_relocation(unit, type, initializer)) {
        relocation->offset = offset;
        image.relocations.push_back(std::move(*relocation));
        return true;
    }

    const auto errors = unit.diagnostics.error_count();
    Node *integer_node = initializer;
    if (initializer->getKind() == cparser::NK_CAST_EXPR) {
        auto *cast = static_cast<cparser::CastExprASTNode *>(initializer);
        auto cast_type = read_type(cast->getType(), unit.source, unit.diagnostics, true);
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
        auto value = integer_constant(integer_node, unit.source, unit.diagnostics);
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

bool initialize_scalar(Unit &unit, const CType &type, Node *initializer,
                       Image &image, std::uint64_t offset) {
    if (type.integer())
        return initialize_integer(unit, type, initializer, image, offset);
    if (type.pointer())
        return initialize_pointer(unit, type, initializer, image, offset);
    unit.error(initializer, "file-scope aggregate initializer is not supported",
               "cephyr.global.type");
    return false;
}

bool initialize_array(Unit &unit, const CType &type, Node *initializer,
                      Image &image) {
    if (!type.element || !type.count) {
        unit.error(initializer, "incomplete array definition is not supported",
                   "cephyr.global.incomplete-array");
        return false;
    }
    if (!type.element->scalar()) {
        unit.error(initializer,
                   "only one-dimensional scalar arrays have supported initializers",
                   "cephyr.global.aggregate");
        return false;
    }

    Node *string_node = initializer;
    bool scalar_form = true;
    if (initializer->getKind() == cparser::NK_LIST) {
        auto list = elements(initializer);
        scalar_form = list.size() == 1;
        if (scalar_form)
            string_node = list.front();
    }
    if (type.element->integer() && type.element->bits == 8 && scalar_form &&
        string_node->getKind() == cparser::NK_STRING_CONST) {
        const auto errors = unit.diagnostics.error_count();
        auto bytes = string_constant(string_node, unit.source, unit.diagnostics);
        if (!bytes && errors == unit.diagnostics.error_count())
            unit.error(string_node, "invalid character-array string initializer",
                       "cephyr.global.string");
        if (!bytes)
            return false;
        const bool omit_terminator =
            bytes->size() == type.count + 1 && !bytes->empty() &&
            bytes->back() == '\0';
        if (bytes->size() > type.count && !omit_terminator) {
            unit.error(string_node, "string literal is too long for character array",
                       "cephyr.global.string-size");
            return false;
        }
        const auto copied =
            std::min<std::uint64_t>(bytes->size(), type.count);
        std::copy_n(bytes->begin(), static_cast<std::size_t>(copied),
                    image.bytes.begin());
        return true;
    }

    if (initializer->getKind() != cparser::NK_LIST) {
        unit.error(initializer, "array initializer must be a brace-enclosed list",
                   "cephyr.global.initializer");
        return false;
    }
    auto values = elements(initializer);
    if (values.size() > type.count) {
        unit.error(initializer, "too many values in array initializer",
                   "cephyr.global.initializer");
        return false;
    }
    const auto element_size = size_of(*type.element);
    if (!element_size) {
        unit.error(initializer, "array element has no supported object size",
                   "cephyr.global.type");
        return false;
    }
    for (std::size_t index = 0; index < values.size(); ++index)
        if (!initialize_scalar(unit, *type.element, values[index], image,
                               static_cast<std::uint64_t>(index) * *element_size))
            return false;
    return true;
}

bool zero_representable(const CType &type) {
    if (type.integer() || type.pointer())
        return true;
    return type.kind == CType::Kind::array && type.count && type.element &&
           zero_representable(*type.element);
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

bool complete_array_type(Unit &unit, Symbol &symbol, Node *initializer) {
    auto &type = symbol.type;
    if (type.kind != CType::Kind::array || type.count)
        return true;
    if (!type.element) {
        unit.error(symbol.definition, "incomplete array has no element type",
                   "cephyr.global.incomplete-array");
        return false;
    }

    if (!present(initializer)) {
        if (symbol.internal) {
            unit.error(symbol.definition,
                       "tentative definition with internal linkage has incomplete array type",
                       "cephyr.global.incomplete-array");
            return false;
        }
        // An external tentative definition of incomplete array type is
        // completed as one element at the end of the translation unit.
        type.count = 1;
        return true;
    }

    Node *string_node = initializer;
    bool scalar_form = true;
    if (initializer->getKind() == cparser::NK_LIST) {
        auto list = elements(initializer);
        scalar_form = list.size() == 1;
        if (scalar_form)
            string_node = list.front();
    }
    if (type.element->integer() && type.element->bits == 8 && scalar_form &&
        string_node->getKind() == cparser::NK_STRING_CONST) {
        const auto errors = unit.diagnostics.error_count();
        auto bytes = string_constant(string_node, unit.source, unit.diagnostics);
        if (!bytes && errors == unit.diagnostics.error_count())
            unit.error(string_node, "invalid incomplete character-array initializer",
                       "cephyr.global.string");
        if (!bytes)
            return false;
        type.count = static_cast<std::uint64_t>(bytes->size());
        return true;
    }

    if (initializer->getKind() != cparser::NK_LIST) {
        unit.error(initializer, "incomplete array requires a list or string initializer",
                   "cephyr.global.incomplete-array");
        return false;
    }
    const auto values = elements(initializer);
    if (values.empty()) {
        unit.error(initializer, "incomplete array initializer is empty",
                   "cephyr.global.incomplete-array");
        return false;
    }
    type.count = values.size();
    return true;
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

bool define_global(Unit &unit, Symbol &symbol) {
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
    if (!complete_array_type(unit, symbol, initializer))
        return false;

    const auto size = size_of(symbol.type);
    const auto alignment = align_of(symbol.type);
    if (!size || !*size || *size > max_object_bytes || !alignment ||
        (alignment & (alignment - 1)) != 0) {
        unit.error(definition, "global object has an unsupported size or alignment",
                   "cephyr.global.layout");
        return false;
    }

    Image image;
    image.size = *size;

    if (!present(initializer)) {
        if (!zero_representable(symbol.type)) {
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
        if (symbol.type.scalar()) {
            if (!initialize_scalar(unit, symbol.type, initializer, image, 0))
                return false;
        } else if (symbol.type.kind == CType::Kind::array) {
            if (!initialize_array(unit, symbol.type, initializer, image))
                return false;
        } else {
            unit.error(initializer, "file-scope aggregate initializer is not supported",
                       "cephyr.global.aggregate");
            return false;
        }
        std::sort(image.relocations.begin(), image.relocations.end(),
                  [](const Relocation &left, const Relocation &right) {
                      return left.offset < right.offset;
                  });
    }

    const auto native_count = static_cast<std::size_t>(*size / 8);
    const bool readonly = read_only(symbol.type);
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
    return true;
}

} // namespace cephyr::detail
