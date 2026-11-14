#include <cmath>
#include <exoml/StaticFFI.hpp>

namespace moonstone::exoml {
namespace {
std::string quote(std::string_view value) { return '"' + json_escape(value) + '"'; }
bool c_name(std::string_view value) {
    static const std::set<std::string_view> keywords = {
        "auto",      "break",    "case",     "char",    "const",     "continue",     "default",
        "do",        "double",   "else",     "enum",    "extern",    "float",        "for",
        "goto",      "if",       "inline",   "int",     "long",      "register",     "restrict",
        "return",    "short",    "signed",   "sizeof",  "static",    "struct",       "switch",
        "typedef",   "union",    "unsigned", "void",    "volatile",  "while",        "class",
        "namespace", "template", "operator", "new",     "delete",    "bool",         "true",
        "false",     "nullptr",  "alignas",  "alignof", "constexpr", "thread_local", "_Bool",
        "_Atomic",   "_Alignas", "_Alignof"};
    return valid_identifier(value) && !keywords.count(value);
}
std::string normalized_type(std::string_view text) {
    std::vector<std::string> words;
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == ' ' || text[i] == '\t') {
            ++i;
            continue;
        }
        if (text[i] == '*') {
            words.emplace_back("*");
            ++i;
            continue;
        }
        auto start = i;
        while (i < text.size() &&
               ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z') ||
                (text[i] >= '0' && text[i] <= '9') || text[i] == '_'))
            ++i;
        if (start == i)
            return {};
        words.emplace_back(text.substr(start, i - start));
    }
    if (words.empty())
        return {};
    std::string base;
    bool pointer = false;
    for (const auto &word : words) {
        if (word == "*") {
            pointer = true;
            continue;
        }
        if (word == "const" || word == "volatile")
            continue;
        if (pointer)
            return {};
        if (!base.empty())
            base += ' ';
        base += word;
    }
    static const std::set<std::string> builtins = {"void",
                                                   "bool",
                                                   "char",
                                                   "signed char",
                                                   "unsigned char",
                                                   "short",
                                                   "short int",
                                                   "signed short",
                                                   "signed short int",
                                                   "unsigned short",
                                                   "unsigned short int",
                                                   "int",
                                                   "signed",
                                                   "signed int",
                                                   "unsigned",
                                                   "unsigned int",
                                                   "long",
                                                   "long int",
                                                   "signed long",
                                                   "signed long int",
                                                   "unsigned long",
                                                   "unsigned long int",
                                                   "long long",
                                                   "long long int",
                                                   "signed long long",
                                                   "signed long long int",
                                                   "unsigned long long",
                                                   "unsigned long long int",
                                                   "float",
                                                   "double",
                                                   "long double"};
    bool valid = builtins.count(base) || c_name(base);
    for (const auto &tag : {"struct ", "union ", "enum "})
        if (base.starts_with(tag) &&
            c_name(std::string_view(base).substr(std::char_traits<char>::length(tag))))
            valid = true;
    if (!valid)
        return {};
    std::string result;
    for (const auto &word : words) {
        if (!result.empty())
            result += ' ';
        result += word;
    }
    return result;
}
bool header_name(std::string_view header) {
    return !header.empty() && std::all_of(header.begin(), header.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-' || c == '.' || c == '/' || c == '+';
    });
}
std::string exported(const CFunction &f) {
    return f.exported_name.empty() ? f.name : f.exported_name;
}
void require_valid(const BindingModule &module) {
    auto errors = validate_binding(module);
    if (!errors.empty()) {
        std::string message = "invalid StaticFFI module";
        for (const auto &item : errors)
            message += "\n  " + item;
        throw std::invalid_argument(message);
    }
}
std::string includes(const BindingModule &module) {
    std::string out;
    for (const auto &header : module.headers)
        out += "#include \"" + header + "\"\n";
    return out;
}
std::string checks(const BindingModule &module) {
    std::ostringstream out;
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        const auto &f = module.functions[i];
        out << "using signature_" << i << " = " << f.return_type << " (*)(";
        for (std::size_t j = 0; j < f.parameters.size(); ++j) {
            if (j)
                out << ", ";
            out << f.parameters[j].c_type;
        }
        if (f.variadic)
            out << (f.parameters.empty() ? "..." : ", ...");
        out << ");\nstatic_assert(std::is_same_v<decltype(&::" << f.name << "), signature_" << i
            << ">, \"StaticFFI signature mismatch: " << f.name << "\");\n";
    }
    for (const auto &layout : module.layouts) {
        out << "static_assert(std::is_standard_layout_v<" << layout.c_type << ">);\n";
        if (layout.size)
            out << "static_assert(sizeof(" << layout.c_type << ") == " << *layout.size
                << ", \"StaticFFI size mismatch\");\n";
        if (layout.alignment)
            out << "static_assert(alignof(" << layout.c_type << ") == " << *layout.alignment
                << ", \"StaticFFI alignment mismatch\");\n";
        for (const auto &field : layout.fields)
            if (field.offset)
                out << "static_assert(offsetof(" << layout.c_type << ", " << field.name
                    << ") == " << *field.offset << ", \"StaticFFI offset mismatch\");\n";
    }
    return out.str();
}
std::string cpp_string(std::string_view value) {
    // Fixed-width octal prevents adjacent digits from extending an escape.
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 32 || c >= 127) {
            result += '\\';
            result += char('0' + (c >> 6));
            result += char('0' + ((c >> 3) & 7));
            result += char('0' + (c & 7));
        } else
            result += char(c);
    }
    return result + '"';
}
std::string value_code(const Value &value) {
    if (auto p = std::get_if<std::string>(&value))
        return "std::string(" + cpp_string(*p) + ", " + std::to_string(p->size()) + ")";
    if (auto p = std::get_if<std::vector<std::string>>(&value)) {
        std::string result = "std::vector<std::string>{";
        for (std::size_t i = 0; i < p->size(); ++i) {
            if (i)
                result += ',';
            result += value_code(Value{(*p)[i]});
        }
        return result + '}';
    }
    if (auto p = std::get_if<std::int64_t>(&value)) {
        if (*p == std::numeric_limits<std::int64_t>::min())
            return "(-INT64_C(9223372036854775807)-1)";
        return "INT64_C(" + std::to_string(*p) + ")";
    }
    if (auto p = std::get_if<std::uint64_t>(&value))
        return "UINT64_C(" + std::to_string(*p) + ")";
    if (std::holds_alternative<double>(value))
        return "double(" + stringify(value) + ")";
    if (std::holds_alternative<bool>(value))
        return stringify(value);
    return "std::monostate{}";
}
std::string indices(const std::set<std::size_t> &values) {
    std::string out = "[";
    for (auto i : values) {
        if (out.size() > 1)
            out += ',';
        out += std::to_string(i);
    }
    return out + ']';
}
} // namespace

Diagnostics binding_diagnostics(const BindingModule &module, BindingLimits limits) {
    Diagnostics errors;
    auto add = [&](std::string code, std::string message, const SourceSpan &source = SourceSpan{}) {
        Diagnostic d;
        d.id = "EXOML-FFI-" + code;
        d.message = std::move(message);
        d.primary = source;
        errors.push_back(std::move(d));
    };
    if (!c_name(module.name))
        add("NAME", "invalid module name");
    if (module.functions.size() > limits.functions || module.constants.size() > limits.functions ||
        module.layouts.size() > limits.functions || module.headers.size() > limits.functions) {
        add("LIMIT", "declaration count budget exceeded");
        return errors;
    }
    std::size_t bytes = 0;
    auto charge = [&](std::string_view text) {
        if (text.size() > limits.text_bytes - std::min(bytes, limits.text_bytes)) {
            bytes = limits.text_bytes;
            return false;
        }
        bytes += text.size();
        return true;
    };
    bool within_budget = charge(module.name);
    std::set<std::string> headers, symbols, exports, layouts;
    for (const auto &header : module.headers) {
        within_budget &= charge(header);
        if (!header_name(header) || !headers.insert(header).second)
            add("HEADER", "invalid or duplicate header: " + header);
    }
    for (const auto &f : module.functions) {
        within_budget &= charge(f.name) && charge(f.return_type) && charge(f.exported_name) &&
                         charge(f.documentation);
        if (!c_name(f.name) || !symbols.insert(f.name).second)
            add("NAME", "invalid or duplicate function: " + f.name, f.source);
        if (!valid_identifier(exported(f)) || !exports.insert(exported(f)).second)
            add("EXPORT", "invalid or duplicate export: " + exported(f), f.source);
        if (normalized_type(f.return_type).empty())
            add("TYPE", "unsupported return declarator: " + f.name, f.source);
        if (f.parameters.size() > limits.parameters) {
            add("LIMIT", "parameter budget exceeded: " + f.name, f.source);
            continue;
        }
        std::set<std::string> params;
        for (const auto &p : f.parameters) {
            within_budget &= charge(p.name) && charge(p.c_type);
            auto type = normalized_type(p.c_type);
            if (!c_name(p.name) || !params.insert(p.name).second || type.empty() ||
                type == "void" || type == "const void")
                add("PARAMETER", "invalid parameter " + p.name + " in " + f.name, f.source);
        }
        for (auto i : f.ownership.consumed_arguments) {
            if (i >= f.parameters.size())
                add("OWNERSHIP", "consumed index out of range: " + f.name, f.source);
            if (f.ownership.borrowed_arguments.count(i))
                add("OWNERSHIP", "argument both consumed and borrowed: " + f.name, f.source);
        }
        for (auto i : f.ownership.borrowed_arguments)
            if (i >= f.parameters.size())
                add("OWNERSHIP", "borrowed index out of range: " + f.name, f.source);
        if (f.ownership.returned_borrow && f.ownership.borrowed_arguments.empty())
            add("OWNERSHIP", "borrowed result has no lifetime source: " + f.name, f.source);
        for (auto effect : f.ownership.effects.effects)
            if (effect < Effect::Read || effect > Effect::Suspend)
                add("EFFECT", "unknown effect: " + f.name, f.source);
    }
    for (const auto &constant : module.constants) {
        within_budget &= charge(constant.name) && charge(stringify(constant.value));
        if (!valid_identifier(constant.name) || !exports.insert(constant.name).second)
            add("EXPORT", "invalid or duplicate constant: " + constant.name);
        if (auto value = std::get_if<double>(&constant.value); value && !std::isfinite(*value))
            add("CONSTANT", "non-finite constant: " + constant.name);
    }
    for (const auto &layout : module.layouts) {
        within_budget &= charge(layout.c_type);
        if (normalized_type(layout.c_type).empty() || !layouts.insert(layout.c_type).second)
            add("LAYOUT", "invalid or duplicate layout type: " + layout.c_type);
        if (layout.size && !*layout.size)
            add("LAYOUT", "zero record size");
        if (layout.alignment &&
            (!*layout.alignment || (*layout.alignment & (*layout.alignment - 1))))
            add("LAYOUT", "alignment must be a nonzero power of two");
        std::set<std::string> fields;
        if (layout.fields.size() > limits.parameters) {
            add("LIMIT", "layout field budget exceeded");
            continue;
        }
        for (const auto &field : layout.fields) {
            within_budget &= charge(field.name);
            if (!c_name(field.name) || !fields.insert(field.name).second)
                add("LAYOUT", "invalid or duplicate layout field: " + field.name);
            if (field.offset && layout.size && *field.offset >= *layout.size)
                add("LAYOUT", "field offset outside record: " + field.name);
        }
    }
    if (!within_budget)
        add("LIMIT", "declaration text budget exceeded");
    return errors;
}
std::vector<std::string> validate_binding(const BindingModule &module) {
    std::vector<std::string> result;
    for (const auto &d : binding_diagnostics(module))
        result.push_back(d.message);
    return result;
}
std::string generate_abi_checks(const BindingModule &module) {
    require_valid(module);
    return "#include <cstddef>\n#include <type_traits>\n" + includes(module) +
           "namespace exoml_check_" + module.name + " {\n" + checks(module) + "}\n";
}
std::string generate_bindings(const BindingModule &module) {
    require_valid(module);
    std::ostringstream out;
    out << "#pragma once\n#include <exoml/StaticFFI.hpp>\n#include <cstddef>\n"
        << includes(module) << "namespace exoml_generated {\ninline void bind_" << module.name
        << "(moonstone::exoml::Interpreter& vm) {\n"
        << checks(module) << "moonstone::exoml::StaticLibrary library(\"" << module.name
        << "\");\n";
    for (const auto &f : module.functions) {
        if (f.variadic)
            throw std::invalid_argument("variadic auto-binding requires an explicit adapter: " +
                                        f.name);
        if (!f.ownership.consumed_arguments.empty() || f.ownership.returned_borrow ||
            f.ownership.may_escape)
            throw std::invalid_argument("ownership policy requires an explicit adapter: " + f.name);
        auto supported = [&](const std::string &type) {
            auto normalized = normalized_type(type);
            if (normalized.find('*') != std::string::npos &&
                !(f.c_strings && normalized == "const char *"))
                throw std::invalid_argument("pointer auto-binding requires an explicit adapter: " +
                                            f.name);
            if (normalized.starts_with("struct ") || normalized.starts_with("union ") ||
                normalized == "long double")
                throw std::invalid_argument("unsupported automatic value conversion: " + type);
        };
        supported(f.return_type);
        for (const auto &p : f.parameters)
            supported(p.c_type);
        out << "{ moonstone::exoml::NativeOptions options;\noptions.privileged = "
            << (f.privileged ? "true" : "false")
            << ";\noptions.thread_affine = " << (f.thread_affine ? "true" : "false")
            << ";\noptions.c_strings = " << (f.c_strings ? "true" : "false") << ";\n"
            << "options.documentation = " << value_code(Value{f.documentation}) << ";\n"
            << "options.ownership.effects.unknown = "
            << (f.ownership.effects.unknown ? "true" : "false") << ";\n";
        for (auto i : f.ownership.borrowed_arguments)
            out << "options.ownership.borrowed_arguments.insert(" << i << ");\n";
        for (auto effect : f.ownership.effects.effects)
            out << "options.ownership.effects.effects.insert(static_cast<moonstone::Effect>("
                << static_cast<int>(effect) << "));\n";
        out << "library.bind(\"" << exported(f) << "\", &::" << f.name << ", options); }\n";
    }
    for (const auto &c : module.constants)
        out << "library.constant(\"" << c.name << "\", moonstone::exoml::Value{"
            << value_code(c.value) << "});\n";
    out << "library.install(vm);\n}\n}\n";
    return out.str();
}
std::string generate_abi_probe(const BindingModule &module) {
    require_valid(module);
    std::ostringstream out;
    out << "#include <cstdio>\n#include <cstddef>\n#include <type_traits>\n"
        << includes(module)
        << "int main() {\nstd::printf(\"{\\\"pointer_size\\\":%zu,\\\"records\\\":[\", "
           "sizeof(void*));\n";
    for (std::size_t i = 0; i < module.layouts.size(); ++i) {
        const auto &layout = module.layouts[i];
        out << "static_assert(std::is_standard_layout_v<" << layout.c_type << ">);\n"
            << "std::printf("
            << cpp_string(std::string(i ? "," : "") + "{\"type\":" + quote(layout.c_type) +
                          ",\"size\":%zu,\"alignment\":%zu,\"fields\":{")
            << ", sizeof(" << layout.c_type << "), alignof(" << layout.c_type << "));\n";
        for (std::size_t j = 0; j < layout.fields.size(); ++j)
            out << "std::printf("
                << cpp_string(std::string(j ? "," : "") + quote(layout.fields[j].name) + ":%zu")
                << ", offsetof(" << layout.c_type << ", " << layout.fields[j].name << "));\n";
        out << "std::printf(\"}}\");\n";
    }
    out << "std::printf(\"]}\\n\");\n}\n";
    return out.str();
}
std::string describe_binding(const BindingModule &module) {
    require_valid(module);
    std::ostringstream out;
    out << "{\"version\":1,\"name\":" << quote(module.name) << ",\"headers\":[";
    for (std::size_t i = 0; i < module.headers.size(); ++i) {
        if (i)
            out << ',';
        out << quote(module.headers[i]);
    }
    out << "],\"functions\":[";
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        if (i)
            out << ',';
        const auto &f = module.functions[i];
        out << "{\"name\":" << quote(f.name) << ",\"export\":" << quote(exported(f))
            << ",\"returns\":" << quote(f.return_type)
            << ",\"variadic\":" << (f.variadic ? "true" : "false")
            << ",\"privileged\":" << (f.privileged ? "true" : "false")
            << ",\"thread_affine\":" << (f.thread_affine ? "true" : "false")
            << ",\"c_strings\":" << (f.c_strings ? "true" : "false")
            << ",\"documentation\":" << quote(f.documentation) << ",\"parameters\":[";
        for (std::size_t j = 0; j < f.parameters.size(); ++j) {
            if (j)
                out << ',';
            out << "{\"name\":" << quote(f.parameters[j].name)
                << ",\"type\":" << quote(f.parameters[j].c_type) << '}';
        }
        out << "],\"ownership\":{\"consumed\":" << indices(f.ownership.consumed_arguments)
            << ",\"borrowed\":" << indices(f.ownership.borrowed_arguments)
            << ",\"returned_borrow\":" << (f.ownership.returned_borrow ? "true" : "false")
            << ",\"may_escape\":" << (f.ownership.may_escape ? "true" : "false")
            << ",\"unknown_effects\":" << (f.ownership.effects.unknown ? "true" : "false")
            << ",\"effects\":[";
        bool first = true;
        for (auto effect : f.ownership.effects.effects) {
            if (!first)
                out << ',';
            first = false;
            out << static_cast<int>(effect);
        }
        out << "]},\"source\":{\"file\":" << quote(f.source.file) << ",\"line\":" << f.source.line
            << ",\"column\":" << f.source.column << ",\"end_line\":" << f.source.end_line
            << ",\"end_column\":" << f.source.end_column << "}}";
    }
    out << "],\"constants\":[";
    for (std::size_t i = 0; i < module.constants.size(); ++i) {
        if (i)
            out << ',';
        const auto &c = module.constants[i];
        out << "{\"name\":" << quote(c.name) << ",\"type\":" << quote(value_type(c.value))
            << ",\"value\":" << quote(stringify(c.value)) << '}';
    }
    out << "],\"layouts\":[";
    for (std::size_t i = 0; i < module.layouts.size(); ++i) {
        if (i)
            out << ',';
        const auto &layout = module.layouts[i];
        out << "{\"type\":" << quote(layout.c_type)
            << ",\"size\":" << (layout.size ? std::to_string(*layout.size) : "null")
            << ",\"alignment\":" << (layout.alignment ? std::to_string(*layout.alignment) : "null")
            << ",\"fields\":[";
        for (std::size_t j = 0; j < layout.fields.size(); ++j) {
            if (j)
                out << ',';
            const auto &field = layout.fields[j];
            out << "{\"name\":" << quote(field.name)
                << ",\"offset\":" << (field.offset ? std::to_string(*field.offset) : "null") << '}';
        }
        out << "]}";
    }
    return out.str() + "]}";
}
void StaticFFI::add(BindingModule module) {
    require_valid(module);
    const auto name = module.name;
    if (!modules_.emplace(name, std::move(module)).second)
        throw std::invalid_argument("duplicate binding module: " + name);
}
const BindingModule &StaticFFI::module(const std::string &name) const {
    auto found = modules_.find(name);
    if (found == modules_.end())
        throw std::invalid_argument("unknown binding module: " + name);
    return found->second;
}
std::vector<std::string> StaticFFI::modules() const {
    std::vector<std::string> result;
    for (const auto &[name, module] : modules_) {
        (void)module;
        result.push_back(name);
    }
    return result;
}
void StaticFFI::install(Interpreter &interpreter) const {
    NativeOptions options;
    options.privileged = false;
    StaticLibrary library("StaticFFI");
    library.bind(
               "Modules", [snapshot = *this] { return snapshot.modules(); }, options)
        .bind(
            "Describe",
            [snapshot = *this](std::string name) {
                return describe_binding(snapshot.module(name));
            },
            options)
        .bind(
            "Generate",
            [snapshot = *this](std::string name) {
                return generate_bindings(snapshot.module(name));
            },
            options)
        .bind(
            "Check",
            [snapshot = *this](std::string name) {
                return generate_abi_checks(snapshot.module(name));
            },
            options)
        .bind(
            "Probe",
            [snapshot = *this](std::string name) {
                return generate_abi_probe(snapshot.module(name));
            },
            options);
    library.install(interpreter);
}
} // namespace moonstone::exoml
